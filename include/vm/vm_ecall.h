/* ============================================================
 *  vm_ecall.h — host-side ECALL routing and the system call ABI
 *
 *  When a guest executes the ecall instruction, the dispatcher
 *  (vm_core.h) returns VM_STEP_ECALL with PC already advanced past
 *  the ecall and the trap state populated. The scheduler then
 *  invokes vm_ecall_dispatch, which looks up a7 in a table of
 *  registered handlers and calls the matching one.
 *
 *  This header defines:
 *    - The syscall number assignments (SYS_*)
 *    - The register ABI for each syscall (in per-call doc comments)
 *    - The error codes handlers may return (VM_E*)
 *    - The handler-registration table API
 *    - The dispatcher entry point
 *
 *  ---------------------------------------------------------------
 *  Register ABI
 *  ---------------------------------------------------------------
 *
 *  Standard RISC-V Linux convention:
 *
 *    a7   syscall number
 *    a0   argument 0       (and return value on success/error)
 *    a1   argument 1
 *    a2   argument 2
 *    a3   argument 3
 *    a4   argument 4
 *    a5   argument 5
 *
 *  Handlers read arguments from cpu->regs[VM_REG_A0..A5] and write
 *  the return value to cpu->regs[VM_REG_A0]. All other registers
 *  are preserved across the syscall (the handler must not modify
 *  a1..a6 or any other register unless the call's contract says
 *  otherwise — and currently no call does).
 *
 *  Returns are signed 32-bit values:
 *    - Non-negative value: success. Meaning is per-call (often 0,
 *      sometimes a count, sometimes a sender ID, etc.).
 *    - Negative value: error. The negated value matches a Linux
 *      errno (see VM_E* below). Guest code can detect errors with
 *      a single signed comparison (a0 < 0).
 *
 *  ---------------------------------------------------------------
 *  Why this ABI shape
 *  ---------------------------------------------------------------
 *
 *  The shape (a7=number, a0..a5=args, a0=return, negated errno on
 *  error) is the actual Linux RISC-V convention. Reusing it means:
 *
 *    - Inline-asm guest stubs look exactly like real Linux syscall
 *      stubs. Anyone who has written Linux syscall wrappers in
 *      RISC-V asm can read these without surprises.
 *
 *    - picolibc, newlib, and similar libc implementations have
 *      pluggable syscall backends. Pointing them at our handlers
 *      is mostly a no-op for the calls they recognize.
 *
 *    - The "negated errno" convention plays nicely with the
 *      classic userspace wrapper pattern:
 *
 *          long r = sys_foo(...);
 *          if (r < 0) { errno = -r; return -1; }
 *          return r;
 *
 *  We do NOT use Linux's actual syscall numbers for VM-specific
 *  calls. SYS_EXIT alone matches Linux (93) so that stock crt0
 *  exit code works without patching. Everything else lives in a
 *  high range (1024+) that will never collide with a Linux number.
 *  Unknown syscalls (numbers with no registered handler) return
 *  -ENOSYS, matching real Linux behavior.
 *
 *  ---------------------------------------------------------------
 *  Handler model
 *  ---------------------------------------------------------------
 *
 *  Handlers are pure functions of (cpu, system). They:
 *
 *    1. Read arguments from cpu->regs[a0..a5].
 *    2. Do their work.
 *    3. Write a return value (success or negated errno) to
 *       cpu->regs[a0].
 *    4. Optionally set scheduler-visible state (block_reason,
 *       halted, etc.) on the CPU if the call has scheduler
 *       implications. The scheduler reads this after the handler
 *       returns and acts accordingly.
 *    5. Return.
 *
 *  Handlers never directly suspend, sleep, or modify the
 *  scheduler's state. They communicate scheduling intent via flags
 *  on the CPU. This keeps the handler API trivial and the
 *  scheduler in charge of all policy.
 *
 *  Handlers are registered at boot via vm_ecall_register. The
 *  table is fixed-size (VM_ECALL_TABLE_SIZE) and shared across all
 *  VMs in the system — every VM dispatches through the same table.
 *  A fallback handler is called for any number with no registered
 *  entry; the default fallback returns -ENOSYS.
 *
 *  ---------------------------------------------------------------
 *  Threading
 *  ---------------------------------------------------------------
 *
 *  Single host thread by design (see vm_core.h). Only one handler
 *  runs at a time, across all VMs in the system. Handlers may
 *  freely read and write shared state (slab allocator, mailbox
 *  list, scheduler queues) without locking.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_ECALL_H
#define VM_ECALL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vm/vm_core.h"   /* for VmCpu, VmEcallHandler */

/* ============================================================
 *  Syscall numbers
 *
 *  SYS_EXIT matches Linux RISC-V (93) so that stock crt0 exit
 *  paths work without patching. Everything else is VM-specific
 *  and lives in the 1024+ range to avoid any Linux collision.
 *
 *  Numbers within a subsystem are grouped by 16 to leave room
 *  for additions without renumbering.
 * ============================================================ */

/* --- Linux-compatible (kept at Linux number for toolchain compat) --- */
#define SYS_READ              63   /* read(fd, buf, n) — host-only,
                                    * not auto-installed; see
                                    * vm_host_stdio.h */
#define SYS_WRITE             64   /* write(fd, buf, n) — host-only,
                                    * not auto-installed; see
                                    * vm_host_stdio.h */
#define SYS_FFLUSH            82   /* fsync(fd) borrowed: flush the
                                    * host FILE* buffer for fd 1 or 2.
                                    * Useful for ANSI sequences and
                                    * other no-newline output that
                                    * needs to be visible immediately.
                                    * a0=fd → 0 on success, -EBADF
                                    * if fd isn't a writable stdio
                                    * stream. fd=0 (stdin) flushes
                                    * both stdout and stderr — handy
                                    * pseudo-syncpoint. */
#define SYS_EXIT              93   /* clean exit (matches Linux RISC-V) */

/* --- Filesystem (Linux RISC-V numbers, host-only, not auto-installed) ---
 *
 * These match Linux's generic ABI numbers so that a guest built
 * against picolibc or a similar libc with Linux-compatible syscall
 * wrappers will work without translation. Like SYS_READ and
 * SYS_WRITE, these are NOT installed by vm_system_init — the host
 * application opts in by calling vm_host_install_fs() (see
 * vm_host_fs.h).
 *
 * Most are *at-style ("openat", "mkdirat", ...) where the dirfd
 * argument is required to be AT_FDCWD (-100), meaning "interpret
 * paths as absolute". The VM has no per-fd current directory.
 *
 * Note: 56 (openat) sits ABOVE the range we use for stdio (63, 64)
 * but is documented here for clustering with the other fs calls. */
#define SYS_MKDIRAT           34   /* mkdirat(AT_FDCWD, path, mode) */
#define SYS_UNLINKAT          35   /* unlinkat(AT_FDCWD, path, flags) */
#define SYS_OPENAT            56   /* openat(AT_FDCWD, path, flags, mode) → fd */
#define SYS_CLOSE             57   /* close(fd) */
#define SYS_LSEEK             62   /* lseek(fd, offset, whence) → new pos */
/* SYS_READ (63) and SYS_WRITE (64) — see above */
#define SYS_READDIR          120   /* VM-specific: readdir(fd, &VmDirent)
                                    * Not Linux's getdents64 — see vm_host_fs.h
                                    * for the simpler dirent layout we use. */

/* --- Identity / introspection (1024..1039) --- */
#define SYS_SELF            1024   /* get this VM's ID */

/* --- Process management (1104..1119) ---
 *
 * Spawning is host-mediated — the guest hands the host an
 * ELF path, the host loads it as a new VM, runs it to
 * completion, and returns the exit code. There's no fork,
 * exec, or async-spawn here; just synchronous run-this-and-
 * wait-for-it.
 *
 * Spawn syscalls are NOT installed by vm_system_init; the host
 * application opts in by calling vm_host_install_fs() (the same
 * module that owns file IO — see vm_host_fs.h).
 */
#define SYS_SPAWN_AND_WAIT  1104   /* spawn_and_wait(path) → exit_code or -errno */

/* --- TTY control (1105..1119) ---
 *
 * Lets a guest toggle the host terminal's raw mode at runtime.
 * Raw mode delivers keystrokes immediately byte-at-a-time, with
 * no echo and no signal-generation (Ctrl-C arrives as 0x03, not
 * SIGINT). Used by interactive guests like games and editors
 * that need to read individual keys including arrow keys
 * (which arrive as the ANSI escape sequence ESC [ A/B/C/D).
 *
 * The host's terminal stays in raw mode only while the guest
 * has it enabled; the host's atexit hook also restores cooked
 * mode on process exit, so a crashed guest doesn't leave the
 * user's terminal broken.
 *
 * NOT installed by vm_system_init or vm_host_install_stdio.
 * The handler is registered by vm_host_install_fs (since the
 * use cases naturally cluster with spawn — guests using TTY
 * control are typically loaded by 'run'). */
#define SYS_TTY_SET_RAW     1105   /* tty_set_raw(enable) → 0 or -errno */

/* --- Memory introspection (1106..1107) ---
 *
 * Report on the host's slab allocators and per-VM memory usage.
 * Used by the shell's 'meminfo' command and by guests that want
 * to monitor their own resource consumption.
 *
 * SYS_SLAB_STATS:
 *   a0 = guest pointer to a VmSlabStatsRecord buffer
 *   a1 = buffer size in bytes (must be >= sizeof(VmSlabStatsRecord))
 *   → a0 = bytes written on success, -EINVAL / -EFAULT on bad args
 *
 * SYS_VM_STATS:
 *   a0 = vm_id, or 0xFFFF to mean "the calling VM"
 *   a1 = guest pointer to a VmVmStatsRecord buffer
 *   a2 = buffer size in bytes (must be >= sizeof(VmVmStatsRecord))
 *   → a0 = bytes written on success, -EINVAL / -EFAULT / -ENOENT
 *
 * Both record shapes are versioned via a leading 'version' field
 * (currently 1). Future additions append fields without changing
 * version unless layout breaks. */
#define SYS_SLAB_STATS      1106
#define SYS_VM_STATS        1107

/* Platform services (1108-1119): small primitives that are
 * libc-shaped but live in the host so guests don't have to
 * carry implementations. Each one is small in code AND state;
 * see vm/vm_host_platform.h for the full contract. */
#define SYS_FORMAT_AND_WRITE          1108  /* (fd, fmt, args, nargs) → bytes_written */
#define SYS_FORMAT_TO_BUF             1109  /* (buf, cap, fmt, args, nargs) → bytes_written */
#define SYS_REALTIME_NOW              1110  /* (out_struct_ptr) → 0 or -ENOSYS */
#define SYS_ALLOC_SIZE                1111  /* (ptr) → block size, or -EINVAL */
#define SYS_RAND                      1112  /* () → next u32 of PRNG output */
#define SYS_TIMING_DEADLINE_REMAINING 1113  /* () → ticks until reload deadline, or 0 */
/* 1114-1119 reserved for future small platform services */

/* TUI service (1132-1147): terminal-canvas drawing as a host
 * service. The canvas lives in the host; the guest sends
 * batched drawing commands via SYS_TUI_FLUSH_DRAW. Lifecycle
 * and presentation get their own direct syscalls.
 *
 * One VM at a time owns the canvas: SYS_TUI_INIT fails with
 * -EBUSY if another VM already owns it. Matches Unix tty
 * foreground semantics. */
#define SYS_TUI_INIT                  1132  /* (rows, cols, flags) → 0 or -EBUSY */
#define SYS_TUI_SHUTDOWN              1133  /* () → 0 (releases ownership) */
#define SYS_TUI_GET_DIMS              1134  /* () → (rows<<16) | cols */
#define SYS_TUI_PRESENT               1135  /* () → 0 */
#define SYS_TUI_PRESENT_DIFF          1136  /* () → 0 */
#define SYS_TUI_POLL_EVENT            1137  /* (event_ptr) → 1 if event, 0 if none */
#define SYS_TUI_FLUSH_DRAW            1138  /* (cmd_buf, byte_len) → 0 or -errno */

/* Tile subsystem (1139-1146): per-VM sub-canvas allocations
 * with blit and grab. See vm_host_tui.h.
 *
 * Handles are opaque (slot << 16) | generation. Destroying and
 * recreating a tile returns a fresh handle, never the same value. */
#define SYS_TUI_TILE_CREATE           1139  /* (rows, cols) → handle or -errno */
#define SYS_TUI_TILE_DESTROY          1140  /* (handle) → 0 or -EBADF */
#define SYS_TUI_TILE_SET              1141  /* (handle, rc_packed, c_attrs_packed, fg, bg) */
#define SYS_TUI_TILE_FILL             1142  /* (handle, c_attrs_packed, fg, bg) → 0 */
#define SYS_TUI_TILE_SET_TRANSPARENT  1143  /* (handle, row, col) → 0 */
#define SYS_TUI_TILE_BLIT             1144  /* (handle, dest_row, dest_col) → 0 */
#define SYS_TUI_TILE_GRAB             1145  /* (handle, src_packed_rc, hw_packed) → 0 */
#define SYS_TUI_TILE_DIMS             1146  /* (handle) → (rows<<16)|cols or -EBADF */
/* 1147 reserved */

/* --- Audio (1160..1175) ---
 *
 * Guests reach the audio co-processor service through these. On the
 * host side each posts a REQ_AUDIO_* message over the service channel
 * to the audio service (which on the H745 runs on the M4); calls that
 * return a handle wait for the response. Two handle kinds (see
 * docs/audio-architecture.md): durable OBJECT handles (loaded
 * samples/music) and transient VOICE handles (a playing instance).
 *
 * Audio is a shared global service; handles are system-wide. The
 * host stamps the calling VM's id as owner so a dying VM's objects
 * and voices are swept automatically. */
#define SYS_AUDIO_LOAD_SAMPLE  1160  /* (buf, size) → object handle or 0 (fail)   */
#define SYS_AUDIO_LOAD_MUSIC   1161  /* (intro_buf,intro_sz,loop_buf,loop_sz)→obj */
#define SYS_AUDIO_FREE         1162  /* (object) → 0; drops the VM's ref          */
#define SYS_AUDIO_TRIGGER_SFX  1163  /* (object, gain_q15, pan_q15) → voice or 0  */
#define SYS_AUDIO_PLAY_MUSIC   1164  /* (object, flags) → voice or 0 (REJECTED)   */
#define SYS_AUDIO_STOP         1165  /* (voice) → 0 or -errno                     */
#define SYS_AUDIO_SET_GAIN     1166  /* (voice, gain_q15) → 0 or -errno           */
#define SYS_AUDIO_GET_LEVELS   1167  /* (out_buf, n_bands) → bands written (meters)*/
#define SYS_AUDIO_FFT_ENABLE   1168  /* (enable) → 0; turn band meters on/off      */
#define SYS_AUDIO_LOAD_WAV     1169  /* (path) → object handle; host parses a .wav */
#define SYS_AUDIO_STREAM_WAV   1170  /* (path) → voice; host streams a long .wav   */
/* PCM streaming voice — guest pushes raw int16 stereo frames into a
 * mixer channel one chunk at a time. Used by FMV2 playback (per-frame
 * audio chunk demuxed from the muxed video file and fed straight to
 * the mixer) and any other live-PCM source (procedural music, network
 * voice, …). The mixer's per-channel source-rate interpolator handles
 * any sample-rate mismatch with the mixer output rate. */
#define SYS_AUDIO_PCM_STREAM_OPEN  1171  /* (rate, channels) → voice or 0           */
#define SYS_AUDIO_PCM_STREAM_FEED  1172  /* (voice, frames_buf, frame_count) → fed  */
#define SYS_AUDIO_PCM_STREAM_CLOSE 1173  /* (voice) → 0 or -errno                   */
/* 1174..1175 reserved for audio */

/* --- Cart coprocessor staging (1180..1199) ---
 *
 * The mgapi cart runtime stages per-frame PPU payload into the 64 KB
 * cart window the SNES side sees. Guests use these to drive an
 * actual SNES (via bsnes-plus mapper on Windows, real cart bus on the
 * M7). NOT installed by vm_system_init; the embedder calls
 * vm_host_install_copro from inside mgapi_vm_init. */
#define SYS_COPRO_STAGE_PAYLOAD   1180  /* (window_off, guest_buf, size) → 0/-errno */
#define SYS_COPRO_STAGE_DMA_SLOT  1181  /* (slot, bbus, dmap, src, size, prep) → 0   */
#define SYS_COPRO_FRAME_COMMIT    1182  /* (frame_ready_byte) → 0                    */
#define SYS_COPRO_READ_PADS       1183  /* (out_buf_4xu16) → 0; latest joypads       */
#define SYS_COPRO_WAIT_VBLANK     1184  /* () → 0; blocks until frame consumed       */

/* L2 allocator (1185..1189): the system-wide bulk-storage pool that
 * lives in the upper half of SHARED (0xE000_0000+). The handler
 * returns a guest VA the caller can dereference directly — the VM
 * translates 0xE000_0000-range accesses into the L2 backing. */
#define SYS_L2_ALLOC              1185  /* (size, align) → guest VA, or 0 (OOM)      */
#define SYS_L2_FREE               1186  /* (guest_va) → 0                            */
#define SYS_L2_STATS              1187  /* (out_struct_ptr) → 0/-errno               */

/* Cart reset awareness (1188..1189):
 * RESET_COUNT lets the guest detect "the SNES was reset" by polling a
 * monotonically-incrementing counter. The handler bumps it each time
 * mgapi_cart_reset_begin runs; guests compare against a snapshot from
 * the prior iteration to decide whether to reload state.
 *
 * RESET_ACK is reserved for a future opt-in "guest is ready, release
 * reset early" handshake; not implemented yet. */
#define SYS_COPRO_RESET_COUNT     1188  /* () → uint32 reset counter (always non-neg) */
#define SYS_COPRO_RESET_ACK       1189  /* RESERVED for future use                    */

/* --- Game-facing cart API (1190..1219), spec at docs/game-api.md ---
 *
 * The mg_* guest library (examples/common/guest/mg_*.{h,c}) wraps these
 * with the customer-facing names (mg_sprite_set, mg_bg_set_tile, etc.).
 * Game programmers don't call SYS_MG_* directly. See docs/game-api.md
 * for the full contract and rationale for each shape decision.
 *
 * Result semantics: calls that DMA return MgResult (0 = MG_OK,
 * negative = MG_ERR_*). Calls that mutate shadow buffers (sprite_set
 * and similar) have no DMA cost and return 0 unconditionally. */

/* OAM (1190..1199) — sprites + slot allocator + snapshot. */
#define SYS_MG_SPRITE_SET         1190  /* (slot, *MgSprite) → 0           */
#define SYS_MG_SPRITE_MOVE        1191  /* (slot, x, y) → 0                */
#define SYS_MG_SPRITE_HIDE        1192  /* (slot) → 0                      */
#define SYS_MG_SPRITE_GET         1193  /* (slot, *out_MgSprite) → 0       */
#define SYS_MG_SPRITES_CLEAR_ALL  1194  /* () → 0                          */
#define SYS_MG_SPRITE_SIZES       1195  /* (MgSpriteSizes pair) → 0        */
#define SYS_MG_SPRITE_CHR_BASE    1196  /* (base0_word, base1_word) → 0    */
#define SYS_MG_SPRITE_ALLOC       1197  /* (count) → first slot or -1      */
#define SYS_MG_SPRITE_FREE        1198  /* (first, count) → 0              */
#define SYS_MG_OAM_SNAP_RESTORE   1199  /* (op, *buf_544): op 0=snap 1=rst */

/* BG (1200..1209) — modes, tilemap shadow + direct, scroll, effects. */
#define SYS_MG_BG_MODE            1200  /* (MgBgMode) → 0                  */
#define SYS_MG_BG_SETUP           1201  /* (layer, tmap_w, size, chr_w)→0  */
#define SYS_MG_BG_ENABLE          1202  /* (layer, main_bit | sub_bit) → 0 */
#define SYS_MG_BG_SET_TILE        1203  /* (layer, x, y, cell_word) → 0    */
#define SYS_MG_BG_GET_TILE        1204  /* (layer, x, y, *out_cell) → 0    */
#define SYS_MG_BG_BLIT            1205  /* (layer, x, y, *cells, n) → 0    */
#define SYS_MG_BG_UPLOAD          1206  /* (layer, x, y, *cells, n) → res  */
#define SYS_MG_BG_SCROLL          1207  /* (layer, hx, vy) → 0             */
#define SYS_MG_BG_MOSAIC          1208  /* (size, layer_mask) → 0          */
#define SYS_MG_BG_MAIN_PRIORITY   1209  /* (layer, hi_flag) → 0            */

/* Mode 7 + HDMA (1210..1214). */
#define SYS_MG_MODE7_SET          1210  /* (*MgMode7Params) → 0            */
#define SYS_MG_MODE7_WRAP         1211  /* (MgMode7Wrap) → 0               */
#define SYS_MG_HDMA_SETUP         1212  /* (*MgHdmaCfg) → 0                */
#define SYS_MG_HDMA_UPLOAD        1213  /* (channel, *table, len) → res    */
#define SYS_MG_HDMA_ENABLE        1214  /* (channel, on_bit) → 0           */

/* GFX (1215..1218) + panic (1219). */
#define SYS_MG_CHR_UPLOAD         1215  /* (vram_word, *src, bytes) → res  */
#define SYS_MG_PALETTE_WRITE      1216  /* (start, count, *src, fmt) → 0
                                         *   fmt: 0=bgr555 word source,
                                         *        1=rgb24 byte source.    */
#define SYS_MG_PALETTE_SNAP_RESTORE 1217 /* (op, *buf_512): 0=snap, 1=rst */
#define SYS_MG_PACK_CHR           1218  /* (*dst, *src_linear, tiles, bpp) */
#define SYS_MG_PANIC              1219  /* (*msg_cstr) → noreturn          */

/* Frame-state introspection + force-blank (1220). One multiplexed
 * ecall to keep the SYS_MG_* range tight; op-code in a0:
 *
 *   op 0  get slots_remaining   → uint8  (0..8)
 *   op 1  get bytes_remaining   → uint16 (0..~6479)
 *   op 2  get force_blank_top   → uint8
 *   op 3  get force_blank_bot   → uint8
 *   op 4  set force_blank       (a1 = top, a2 = bottom) → 0
 */
#define SYS_MG_FRAME_STATE        1220

/* Opt-in PPU reset (1221). A child VM calls this when it wants to
 * start from a known-zero PPU state regardless of what the parent left
 * behind. The next mg_frame_commit's DMA list will include CGRAM/OAM
 * full-zero writes plus a VRAM-fill slot that wipes all 32K VRAM words
 * to $0000. Skip the call if you want to inherit parent graphics
 * (e.g., for sub-window blits or shared-overlay UIs). */
#define SYS_MG_PPU_CLEAN_SLATE    1221  /* () → 0 */

/* --- Stream arbiter (1223..1226) ---
 *
 * Guest registers an already-open vm_host_fs fd as a streaming
 * source with the host-side round-robin arbiter (see
 * include/io/stream_arbiter.h). The arbiter reads chunk_bytes at
 * a time into a per-stream SPSC ring, ahead of the guest's
 * consumption — so the guest's per-iter consume returns
 * immediately if the ring has a chunk staged, or returns "empty"
 * for the guest to poll with sleep_ticks.
 *
 * Motivation: matches the music-stream pattern (file→ring→mixer)
 * for any guest, and on the MCU port becomes the single SD-bus
 * scheduler that prevents one stream from starving another.
 */
#define SYS_STREAM_REGISTER  1223  /* (fd, chunk_bytes, depth) → handle/-errno */
#define SYS_STREAM_CONSUME   1224  /* (handle, dst, dst_cap) → bytes copied (=chunk_bytes), 0=empty, -1=EOF */
#define SYS_STREAM_CLOSE     1225  /* (handle) → 0 or -errno */
#define SYS_STREAM_EOF       1226  /* (handle) → 1 if drained and EOF, else 0 */

/* v2.18: install a per-app custom NMI handler. Guest passes a buffer
 * of 65816 machine code; host copies into the cart-window NMI region
 * and bumps a version counter the SNES kernel polls in @loop. The
 * kernel copies to WRAM at $0E00 and updates RAMVEC_NMI on next poll
 * (within one frame). Handler bytes are run from WRAM (no cart-bus
 * read per instruction). See include/mg_nmi.h for the builder API.
 *   args: (const void *code, uint32_t size)
 *   ret:  0 on success; -EINVAL if size > 1024 or code is bad. */
#define SYS_MG_NMI_INSTALL   1227  /* (code, size) → 0 or -errno */
#define SYS_MG_HIRQ_INSTALL  1228  /* (code, size) → 0 or -errno, v2.26 */
#define SYS_MG_HIRQ_CONFIGURE 1229 /* (vtime, htime, nmitimen_bits) → 0, v2.26 */
#define SYS_MG_KERNEL_LAYOUT  1230 /* (top_lb, bottom_lb) → 0, v2.29 Phase 3a */
#define SYS_MG_SIPHON_CONFIGURE 1231 /* (bytes_per_line, src_off, wram_dst) → 0 */

/* v2.40: host-driven FMV playback. The guest opens the .fmv via fs_open
 * and hands the fd to SYS_FMV_PLAY; the host FMV player parses the header,
 * spins up the FMV_VIDEO stream producer (frames built ahead into a ring),
 * and drives the cart window directly from the FRAME_DONE consumer. The
 * player owns the fd after PLAY (closes it on STOP). The guest loop shrinks
 * to: play; while(status != EOF){ poll input; mg_wait_frame; }; stop. */
#define SYS_FMV_PLAY    1232  /* (fd) → 0 or -errno; player takes the fd */
#define SYS_FMV_STOP    1233  /* () → 0 */
#define SYS_FMV_STATUS  1234  /* () → 0 idle / 1 playing / 2 eof */
#define SYS_FMV_SET_HTIME 1235 /* (htime 1..254) → 0; live siphon force-blank tune */

/* --- Cooperative scheduling (1040..1055) --- */
#define SYS_YIELD           1040   /* relinquish remainder of quantum */
#define SYS_CRITICAL_ENTER  1041   /* begin non-preemptible region */
#define SYS_CRITICAL_EXIT   1042   /* end non-preemptible region */

/* --- Timer / clock (1043..1055) ---
 *
 * Provide a guest-visible monotonic tick counter and primitives
 * for sleeping until a deadline. The tick is owned by the host:
 * the scheduler optionally reads from a host-provided callback
 * (typically a millisecond SysTick on a microcontroller, or
 * clock_gettime on a PC). See VmSystemConfig.tick_source.
 *
 * Tick semantics:
 *   - global_tick is uint32; it can wrap (about 49.7 days at
 *     1 ms). Wraparound-safe comparisons are used internally.
 *   - SYS_TICK_HZ returns the configured ticks-per-second (e.g.,
 *     1000 for a 1 ms tick) or 0 if the host hasn't configured a
 *     tick_source. A guest seeing 0 should fall back to "ticks
 *     are abstract"; useful for diagnostics.
 *   - Granularity is "close enough": cooperative scheduling means
 *     a sleeping VM wakes whenever the scheduler next gets to it
 *     after the deadline, not at the deadline exactly. Slack is
 *     bounded by quantum length and other VMs' work.
 *
 * Two flavors of periodic loop are supported. Pick whichever
 * fits the use case.
 *
 * Flavor A — explicit absolute deadline, guest-managed:
 *
 *     uint32_t period = sys_tick_hz() / 10;     // 10 Hz
 *     uint32_t next   = sys_ticks_now() + period;
 *     while (!quit) {
 *         do_frame();
 *         sys_sleep_until(next);
 *         next += period;
 *     }
 *
 * SYS_SLEEP_UNTIL with an absolute deadline keeps phase exactly
 * — a slow frame doesn't accumulate drift across iterations.
 * The guest carries the `next` variable in its hot loop.
 *
 * Flavor B — kernel-managed auto-reload (preferred when the
 * period is constant):
 *
 *     sys_set_reload_period(125);              // 125 ticks / frame
 *     while (!quit) {
 *         sys_yield_until_reload();
 *         do_frame();
 *     }
 *
 * The deadline lives in the kernel (per VmCpu). No `next +=`
 * arithmetic in the guest, one fewer register live across the
 * sleep, one fewer chance of an off-by-one. If a frame overruns
 * by more than a period, the kernel skips ahead to the next
 * FUTURE boundary — phase preserved, missed frames dropped
 * cleanly (no burst-catch-up).
 *
 * The reload and explicit-deadline mechanisms are independent;
 * a guest can use one or the other or both. SYS_SLEEP_* does
 * not touch the reload state; SYS_SET_RELOAD_PERIOD does not
 * touch BLOCK_SLEEP. */
#define SYS_TICKS_NOW           1043   /* () → current global_tick */
#define SYS_TICK_HZ             1044   /* () → ticks_per_second (0 if unset) */
#define SYS_SLEEP_TICKS         1045   /* (n) → block for n ticks; 0 = yield */
#define SYS_SLEEP_UNTIL         1046   /* (deadline) → block until tick >= deadline */
#define SYS_SET_RELOAD_PERIOD   1047   /* (period) → 0; period=0 clears */
#define SYS_YIELD_UNTIL_RELOAD  1048   /* () → block until next reload boundary */

/* --- Shared-region allocator (1056..1071) --- */
#define SYS_ALLOC           1056   /* allocate from shared region */
#define SYS_FREE            1057   /* free to shared region */

/* --- Mailbox messaging (1072..1087) --- */
#define SYS_SEND            1072   /* send message to a VM's mailbox */
#define SYS_RECV            1073   /* receive a message from own mailbox */
#define SYS_MAILBOX_INFO    1074   /* query a target VM's mailbox shape */
#define SYS_WHITELIST_ADD   1075   /* allow a sender to message us */
#define SYS_WHITELIST_REMOVE 1076  /* revoke a sender's permission */

/* --- libc memory/string acceleration (1088..1119, 32 slots) ---
 *
 * These exist so the guest's libc can implement memcpy/memcmp/
 * strlen/etc. as a thin ecall wrapper. The host runs a native
 * implementation, which is dramatically faster than the guest
 * interpreting an instruction-at-a-time loop. The crossover
 * point where the ecall pays for itself is small (a few dozen
 * bytes); below that the guest's compiled loop would win, so
 * libc wrappers can short-circuit small sizes if they care.
 *
 * All pointers are guest addresses; the host translates and
 * bounds-checks before doing the operation. Operations that
 * straddle region boundaries are rejected as -EFAULT — each
 * call must operate within a single region. */
#define SYS_MEMCPY          1088   /* (dst, src, n) → 0 or -err */
#define SYS_MEMSET          1089   /* (dst, byte, n) → 0 or -err */
#define SYS_MEMMOVE         1090   /* (dst, src, n) → 0 or -err */
#define SYS_MEMCMP          1091   /* (a, b, n) → sign(memcmp) or -err */
#define SYS_STRLEN          1092   /* (s, maxlen) → length or -err */
#define SYS_STRCMP          1093   /* (a, b, maxlen) → sign(strcmp) or -err */
#define SYS_STRCHR          1094   /* (s, c, maxlen) → offset or -ENOENT */

/* --- Fixed-point math acceleration (1120..1151, 32 slots) ---
 *
 * The guest's working numeric format is fixed-point (see
 * math/fixed_point.h). These syscalls accelerate transcendental
 * functions on q16_16_t values — operations the guest's own
 * fixed_point module doesn't provide because a guest-side
 * implementation would be a polynomial-approximation loop
 * dozens of instructions long, interpreted slowly.
 *
 * Inputs and outputs are q16_16_t passed as int32_t in a0 (and
 * sometimes a1 for two-argument calls). The host implements
 * these however it likes — using hardware double-precision via
 * <math.h>, using a fixed-point CORDIC, or a lookup table — and
 * the guest sees only fixed-point in, fixed-point out.
 *
 * No floats cross the ABI: floats only appear in the optional
 * conversion syscalls below, which exist for the rare case
 * where a guest must consume float-formatted external data. */
#define SYS_FIX_SIN         1120   /* angle (q16_16) → sin */
#define SYS_FIX_COS         1121   /* angle (q16_16) → cos */
#define SYS_FIX_TAN         1122   /* angle (q16_16) → tan */
#define SYS_FIX_ATAN2       1123   /* (y, x) q16_16 → atan2 */
#define SYS_FIX_SQRT        1124   /* value (q16_16, >=0) → sqrt */
#define SYS_FIX_EXP         1125   /* value (q16_16) → e^value */
#define SYS_FIX_LOG         1126   /* value (q16_16, >0) → ln(value) */
#define SYS_FIX_POW         1127   /* (base, exp) q16_16 → base^exp */

/* Format conversions for external-data boundaries. Most guests
 * don't need these — compile-time float constants should be
 * expressed via Q16_16_FROM_DOUBLE() in source, which evaluates
 * to a baked-in integer constant with no runtime conversion.
 * These syscalls are for the runtime case (parsing a float from
 * a file, receiving an IEEE-formatted sample over a wire, etc.). */
#define SYS_FLOAT_TO_FIX    1128   /* IEEE single in a0 → q16_16 */
#define SYS_FIX_TO_FLOAT    1129   /* q16_16 in a0 → IEEE single */
#define SYS_DOUBLE_TO_FIX   1130   /* IEEE double (a0:a1) → q16_16 */
#define SYS_FIX_TO_DOUBLE   1131   /* q16_16 → IEEE double (a0:a1) */

/* ============================================================
 *  Reserved syscall ranges for plugins
 *
 *  These ranges are reserved in the ABI but unimplemented in
 *  the public-domain build. Plugin code (vendor or third-party)
 *  can register handlers for these numbers via the standard
 *  vm_ecall_register mechanism. A guest that calls one of these
 *  without a plugin registered gets the default "unhandled
 *  syscall" path (-ENOSYS).
 *
 *  Ranges:
 *    1200-1219   Cryptography (hash, encrypt, sign, verify, etc.)
 *                  Vendor-supplied. Public-domain build ships no
 *                  primitives in this range.
 *    1220-1239   Open-source plugin space. Reserved for community
 *                  plugins to avoid clashing with vendor namespace.
 *    1240-1259   Vendor-specific extensions. Each vendor can
 *                  define their own conventions within this range.
 *
 *  See include/vm/vm_plugin.h for the plugin-registration API.
 * ============================================================ */
/* (No #defines for the reserved ranges — leave the names to
 * the plugin author so the public ABI doesn't accidentally
 * commit to specific semantics.) */

/* ============================================================
 *  Per-syscall ABI documentation
 *
 *  Format: arguments in a0..a5 (only those actually used), return
 *  in a0. "→" marks return values. Errors are always negative
 *  numbers matching the VM_E* errno values defined below.
 *
 *  -----------------------------------------------------------
 *  SYS_READ          (a7 = 63)  [host-only, not auto-installed]
 *  -----------------------------------------------------------
 *    a0 = file descriptor (0 = stdin; others → -EBADF)
 *    a1 = guest address of buffer to fill
 *    a2 = maximum byte count
 *    → a0 = number of bytes read (0 = nothing available right now,
 *           NOT end-of-stream — call again later)
 *    → a0 = -EBADF  if fd is not 0
 *    → a0 = -EFAULT if [a1, a1+a2) is not writable in the caller
 *    → a0 = -EIO    if the underlying stream signaled a hard error
 *
 *  Non-blocking by design. A return of 0 means "no bytes ready",
 *  not "stream closed" — the guest should poll again (typically
 *  after SYS_YIELD). This matches game-loop / TUI patterns where
 *  the guest can't afford to block waiting for input.
 *
 *  When the underlying stream actually does close (e.g., the
 *  remote end of a pipe), subsequent reads return -EIO. The guest
 *  can distinguish "no input yet" (0) from "input source gone"
 *  (-EIO) by checking the return value.
 *
 *  Like SYS_WRITE, this is NOT installed by vm_system_init.
 *  vm_host_install_stdio() registers both. A system with no host
 *  stdin (pure embedded) just doesn't install it, and SYS_READ
 *  returns -ENOSYS to the guest via the default fallback.
 *
 *  Whether the stream is raw or cooked is a host-side decision
 *  made when stdio is installed (see VmHostStdioConfig.raw_mode).
 *  The guest cannot change this — it gets whatever bytes the host
 *  hands it. For interactive TUI / game-style input, the host
 *  enables raw mode so each keystroke arrives immediately as
 *  bytes (arrow keys as ESC [ A etc., per xterm conventions).
 *
 *  -----------------------------------------------------------
 *  SYS_WRITE         (a7 = 64)  [host-only, not auto-installed]
 *  -----------------------------------------------------------
 *    a0 = file descriptor (1 = stdout, 2 = stderr; others → -EBADF)
 *    a1 = guest address of bytes to write
 *    a2 = byte count
 *    → a0 = number of bytes written on success
 *    → a0 = -EBADF  if fd is not 1 or 2
 *    → a0 = -EFAULT if [a1, a1+a2) is not readable in the caller
 *
 *  This syscall is NOT installed by vm_system_init. A host that
 *  wants to expose its stdout to guests calls
 *  vm_host_install_stdio(sys) (see vm_host_stdio.h) to register
 *  the handler. Embedded firmware that has no host stdout simply
 *  doesn't install it, and SYS_WRITE returns -ENOSYS (the default
 *  fallback) to the guest.
 *
 *  -----------------------------------------------------------
 *  SYS_EXIT          (a7 = 93)
 *  -----------------------------------------------------------
 *    a0 = exit code (informational; not currently propagated
 *                    beyond setting cpu->halted = true)
 *    → does not return (cpu->halted is set, next vm_step returns
 *      VM_STEP_HALTED)
 *
 *  -----------------------------------------------------------
 *  SYS_SELF          (a7 = 1024)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = this VM's vm_id (always non-negative)
 *
 *  -----------------------------------------------------------
 *  SYS_YIELD         (a7 = 1040)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = 0 always
 *    Side effect: the scheduler should treat this as "remainder
 *    of quantum is voluntarily relinquished" and rotate to the
 *    next VM. The handler sets cpu->block_reason = BLOCK_YIELDED.
 *
 *  -----------------------------------------------------------
 *  SYS_CRITICAL_ENTER  (a7 = 1041)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = 0 on success
 *    → a0 = -EBUSY if already in a critical section (no nesting)
 *    Side effect: sets cpu->in_critical = true. The scheduler
 *    will not rotate away from this VM at quantum expiry until
 *    SYS_CRITICAL_EXIT (or a trap) clears the flag.
 *
 *  -----------------------------------------------------------
 *  SYS_CRITICAL_EXIT   (a7 = 1042)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = 0 on success
 *    → a0 = -EINVAL if not currently in a critical section
 *    Side effect: clears cpu->in_critical.
 *
 *  -----------------------------------------------------------
 *  SYS_ALLOC         (a7 = 1056)
 *  -----------------------------------------------------------
 *    a0 = size in bytes (must be > 0 and <= max bin size)
 *    → a0 = guest address in shared region (always 0xC0000000 +
 *           offset, never NULL on success)
 *    → a0 = -ENOMEM if the slab can't satisfy the request
 *    → a0 = -EINVAL if size is 0 or unreasonably large
 *
 *  -----------------------------------------------------------
 *  SYS_FREE          (a7 = 1057)
 *  -----------------------------------------------------------
 *    a0 = guest address previously returned by SYS_ALLOC
 *    → a0 = 0 on success
 *    → a0 = -EINVAL if address is not in the shared region or
 *           does not correspond to a live allocation (double-free,
 *           foreign pointer, corrupted header)
 *
 *  -----------------------------------------------------------
 *  SYS_SEND          (a7 = 1072)
 *  -----------------------------------------------------------
 *    a0 = target vm_id
 *    a1 = guest address of payload (in sender's region 10)
 *    a2 = payload size in bytes (must match target's slot_size)
 *    → a0 = 0 on success
 *    → a0 = -ENOENT  if target vm_id does not exist
 *    → a0 = -EPERM   if sender is not on target's whitelist
 *    → a0 = -EINVAL  if size != target's slot_size
 *    → a0 = -EFAULT  if payload pointer is not in valid memory
 *    → a0 = -EAGAIN  if target's mailbox is full (reject-on-full)
 *
 *  -----------------------------------------------------------
 *  SYS_RECV          (a7 = 1073)
 *  -----------------------------------------------------------
 *    a0 = guest address to write payload to (in this VM's
 *         region 10; must have at least slot_size bytes available)
 *    a1 = timeout in step-quanta (0 = poll only, UINT32_MAX = wait
 *         forever)
 *    → a0 = sender's vm_id (non-negative, ≤ 65535) on success
 *    → a0 = -EAGAIN    if timeout=0 and mailbox is empty
 *    → a0 = -ETIMEDOUT if timeout > 0 and elapsed before a message
 *                       arrived
 *    → a0 = -EFAULT    if destination pointer is not in valid
 *                       memory
 *    → a0 = -EBUSY     if called while in a critical section with
 *                       timeout > 0 (blocking inside critical is
 *                       forbidden; use timeout=0 to poll)
 *    Side effect (only when blocking and not immediately satisfied):
 *    the handler sets cpu->block_reason = BLOCK_MAILBOX_RECV and
 *    stores enough state on the CPU for the scheduler / mailbox
 *    delivery code to write the result when a message arrives or
 *    the timeout expires.
 *
 *  -----------------------------------------------------------
 *  SYS_MAILBOX_INFO  (a7 = 1074)
 *  -----------------------------------------------------------
 *    a0 = target vm_id
 *    → a0 = slot_size of target's mailbox in bytes (positive)
 *    → a0 = -ENOENT if target does not exist
 *    → a0 = -EPERM  if caller is not on target's whitelist
 *    → a1 = current depth available in target's mailbox (free
 *           slots), useful for "should I bother sending"
 *    Note: a1 is meaningful only on success.
 *
 *  -----------------------------------------------------------
 *  SYS_WHITELIST_ADD     (a7 = 1075)
 *  SYS_WHITELIST_REMOVE  (a7 = 1076)
 *  -----------------------------------------------------------
 *    a0 = sender vm_id to allow / revoke
 *    → a0 = 0 on success
 *    → a0 = -ENOENT if sender vm_id is out of range
 *    Whitelist is per-receiver (i.e., this VM). Modifies only
 *    its own permissions, not the sender's.
 *
 *  -----------------------------------------------------------
 *  SYS_MEMCPY          (a7 = 1088)
 *  -----------------------------------------------------------
 *    a0 = dst guest address
 *    a1 = src guest address
 *    a2 = n bytes
 *    → a0 = 0 on success
 *    → a0 = -EFAULT if either range is invalid or straddles regions
 *    Overlapping regions are not defined (use SYS_MEMMOVE).
 *
 *  -----------------------------------------------------------
 *  SYS_MEMSET          (a7 = 1089)
 *  -----------------------------------------------------------
 *    a0 = dst guest address
 *    a1 = byte value (only low 8 bits used)
 *    a2 = n bytes
 *    → a0 = 0 on success
 *    → a0 = -EFAULT on bad range
 *
 *  -----------------------------------------------------------
 *  SYS_MEMMOVE         (a7 = 1090)
 *  -----------------------------------------------------------
 *    a0 = dst guest address
 *    a1 = src guest address
 *    a2 = n bytes
 *    → a0 = 0 on success
 *    → a0 = -EFAULT on bad range
 *    Handles overlap correctly (uses host memmove).
 *
 *  -----------------------------------------------------------
 *  SYS_MEMCMP          (a7 = 1091)
 *  -----------------------------------------------------------
 *    a0 = a guest address
 *    a1 = b guest address
 *    a2 = n bytes
 *    → a0 = -1, 0, or 1 (sign of memcmp result)
 *    → a0 = -EFAULT on bad range
 *    Note: clamped to -1/0/1 rather than the raw memcmp delta
 *    so the return value never collides with the -EFAULT range.
 *
 *  -----------------------------------------------------------
 *  SYS_STRLEN          (a7 = 1092)
 *  -----------------------------------------------------------
 *    a0 = s guest address
 *    a1 = maxlen (search bound, prevents runaway on missing NUL)
 *    → a0 = string length in bytes (0 to maxlen-1)
 *    → a0 = maxlen if no NUL found within maxlen bytes
 *    → a0 = -EFAULT if s..s+maxlen is not in valid memory
 *
 *  -----------------------------------------------------------
 *  SYS_STRCMP          (a7 = 1093)
 *  -----------------------------------------------------------
 *    a0 = a guest address
 *    a1 = b guest address
 *    a2 = maxlen (search bound on both strings)
 *    → a0 = -1, 0, or 1 (sign of strcmp result, clamped)
 *    → a0 = -EFAULT on bad range
 *    Stops at min(strlen(a), strlen(b), maxlen).
 *
 *  -----------------------------------------------------------
 *  SYS_STRCHR          (a7 = 1094)
 *  -----------------------------------------------------------
 *    a0 = s guest address
 *    a1 = character to find (only low 8 bits used)
 *    a2 = maxlen (search bound)
 *    → a0 = offset from s where character was found (0 to maxlen-1)
 *    → a0 = -ENOENT if not found within maxlen
 *    → a0 = -EFAULT if range is invalid
 *    Returns an OFFSET, not an absolute address, so the result
 *    can't collide with the negative error range.
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_SIN / COS / TAN     (a7 = 1120 / 1121 / 1122)
 *  -----------------------------------------------------------
 *    a0 = angle (q16_16, radians)
 *    → a0 = sin / cos / tan of angle (q16_16)
 *    Wraps modulo 2*pi internally; any finite input is accepted.
 *    SYS_FIX_TAN: undefined behavior near pi/2 + k*pi (host
 *    decides; typically returns saturated q16_16 max or min).
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_ATAN2       (a7 = 1123)
 *  -----------------------------------------------------------
 *    a0 = y (q16_16)
 *    a1 = x (q16_16)
 *    → a0 = atan2(y, x) (q16_16, radians, in [-pi, pi])
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_SQRT        (a7 = 1124)
 *  -----------------------------------------------------------
 *    a0 = value (q16_16, must be >= 0)
 *    → a0 = sqrt(value) (q16_16)
 *    → a0 = -EINVAL if value < 0
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_EXP         (a7 = 1125)
 *  -----------------------------------------------------------
 *    a0 = value (q16_16)
 *    → a0 = e^value (q16_16, saturated to q16_16 max on overflow)
 *    The overflow point is around value ≈ 10.4 in q16_16.
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_LOG         (a7 = 1126)
 *  -----------------------------------------------------------
 *    a0 = value (q16_16, must be > 0)
 *    → a0 = ln(value) (q16_16)
 *    → a0 = -EINVAL if value <= 0
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_POW         (a7 = 1127)
 *  -----------------------------------------------------------
 *    a0 = base (q16_16)
 *    a1 = exp  (q16_16)
 *    → a0 = base^exp (q16_16, saturated on overflow)
 *    → a0 = -EINVAL if base < 0 and exp is not an integer
 *           (matches host pow() behavior on negative base)
 *
 *  -----------------------------------------------------------
 *  SYS_FLOAT_TO_FIX / SYS_FIX_TO_FLOAT    (1128 / 1129)
 *  -----------------------------------------------------------
 *    a0 = IEEE single (bit pattern) OR q16_16
 *    → a0 = q16_16 OR IEEE single (bit pattern)
 *    Out-of-range float-to-fix saturates to q16_16 min/max.
 *    NaN converts to 0 (debatable choice; documented as such).
 *
 *  -----------------------------------------------------------
 *  SYS_DOUBLE_TO_FIX / SYS_FIX_TO_DOUBLE  (1130 / 1131)
 *  -----------------------------------------------------------
 *    Same as the single-precision pair but the double is split
 *    across a0:a1 (low 32 bits in a0, high 32 bits in a1) per
 *    the standard RV32 calling convention for 64-bit values.
 *    Out-of-range and NaN: same handling as the single version.
 * ============================================================ */

/* ============================================================
 *  Error codes (negated errno values)
 *
 *  Returned in a0 as negative numbers. Values match the canonical
 *  Linux errno values so familiar names work as expected and so
 *  picolibc's errno-mapping code is happy.
 *
 *  This is NOT the full Linux errno set — just the ones any
 *  handler actually returns. New errors added here as new
 *  handlers need them.
 * ============================================================ */

#define VM_EPERM           1   /* not permitted (whitelist denial) */
#define VM_ENOENT          2   /* no such entity (vm_id, address)  */
#define VM_EIO             5   /* I/O error (stream closed, hw fail) */
#define VM_EBADF           9   /* bad file descriptor (read/write)  */
#define VM_EAGAIN         11   /* try again later (mailbox full, poll empty) */
#define VM_ENOMEM         12   /* out of memory (slab exhausted)   */
#define VM_EFAULT         14   /* bad address (out-of-bounds ptr)  */
#define VM_EBUSY          16   /* resource busy (block-in-critical) */
#define VM_EEXIST         17   /* file already exists              */
#define VM_ENOTDIR        20   /* not a directory                  */
#define VM_EISDIR         21   /* is a directory                   */
#define VM_EINVAL         22   /* invalid argument                  */
#define VM_EMFILE         24   /* too many open files               */
#define VM_ENOSPC         28   /* no space left on device           */
#define VM_EROFS          30   /* read-only filesystem              */
#define VM_ENOSYS         38   /* function not implemented          */
#define VM_ENAMETOOLONG   36   /* path component too long           */
#define VM_ENOTEMPTY      39   /* directory not empty (rmdir)       */
#define VM_ETIMEDOUT     110   /* operation timed out               */

/* ============================================================
 *  Handler table
 *
 *  The dispatcher uses two flat arrays of 256 slots each, covering
 *  the two ranges of syscall numbers we care about:
 *
 *    linux_slots[0..255]:   numbers 0..255 (Linux-compat range;
 *                           today only SYS_EXIT at 93 is used)
 *    vm_slots[0..255]:      numbers 1024..1279 (our VM-specific
 *                           range; current syscalls all fit here,
 *                           with plenty of headroom for additions)
 *
 *  Any other syscall number routes to the fallback handler, which
 *  defaults to returning -ENOSYS in a0.
 *
 *  Memory cost: 512 slots × sizeof(VmEcallHandler) → 2 KB on a
 *  32-bit host. Dispatch cost: one range check, one array index,
 *  one indirect call. No hashing, no chain walks, no allocations.
 *
 *  The two ranges were chosen because they cleanly separate two
 *  classes of syscall:
 *    - low numbers: Linux-compatible (SYS_EXIT must be 93 for
 *      stock crt0; other Linux numbers can be added if you ever
 *      want to plug into existing libc syscall stubs unchanged).
 *    - 1024+ numbers: VM-specific, no risk of collision with
 *      Linux numbers should the libc-compat surface grow.
 *
 *  If you need numbers outside these two ranges, route via the
 *  fallback handler and dispatch yourself; the router does not
 *  support arbitrary numbers by design.
 *
 *  The per-handler signature is VmEcallHandler from vm_core.h:
 *  void (*)(VmCpu*, void*). The second parameter is the 'system'
 *  pointer passed to vm_ecall_dispatch, NOT a per-slot context —
 *  every handler in this router sees the same system pointer.
 *  Handlers that need additional state should reach it through
 *  the system pointer (typically a VmSystem struct holding the
 *  slab allocator, mailbox table, scheduler queues, etc.).
 * ============================================================ */

#define VM_ECALL_LINUX_RANGE_START      0
#define VM_ECALL_LINUX_RANGE_SIZE     256   /* covers 0..255       */
#define VM_ECALL_VM_RANGE_START      1024
#define VM_ECALL_VM_RANGE_SIZE        256   /* covers 1024..1279   */

typedef struct {
    VmEcallHandler  linux_slots[VM_ECALL_LINUX_RANGE_SIZE];
    VmEcallHandler  vm_slots   [VM_ECALL_VM_RANGE_SIZE];
    VmEcallHandler  fallback;  /* called for any other number     */
} VmEcallRouter;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize a router. Clears all slots; sets fallback to a
 * default that writes -ENOSYS to a0. Safe to call on a struct
 * that has not been zero-initialized. */
void vm_ecall_router_init(VmEcallRouter *r);

/* Register a handler for the given syscall number. The handler
 * receives (cpu, system) where 'system' is the pointer passed to
 * vm_ecall_dispatch (typically a VmSystem struct holding all
 * shared state). There is no per-slot context — every handler
 * sees the same system pointer.
 *
 * syscall_num must be in one of the two supported ranges (0..255
 * or 1024..1279). Numbers outside these ranges return false.
 *
 * Returns false if syscall_num is out of range or if a handler
 * is already registered (use vm_ecall_unregister first). */
bool vm_ecall_register(VmEcallRouter *r,
                       uint32_t syscall_num,
                       VmEcallHandler handler);

/* Unregister. Returns false if syscall_num is out of range or
 * no handler is registered. Safe to call on empty slots. */
bool vm_ecall_unregister(VmEcallRouter *r, uint32_t syscall_num);

/* Override the fallback handler (called for any number with no
 * registered handler, or any number outside the two supported
 * ranges). The default writes -ENOSYS to a0. */
void vm_ecall_set_fallback(VmEcallRouter *r, VmEcallHandler fallback);

/* ============================================================
 *  Dispatch
 *
 *  Call this from the scheduler when vm_step returns VM_STEP_ECALL.
 *  Reads cpu->regs[a7], looks up the handler, calls it with the
 *  cpu and system pointers. The handler reads args from
 *  cpu->regs[a0..a5] and writes the result to cpu->regs[a0].
 *
 *  'system' is the caller-defined system context. By convention
 *  it points to the VmSystem struct from vm_system.h (when that
 *  exists). The router itself does not interpret it; it just
 *  forwards it to the handler.
 *
 *  After dispatch, the scheduler should inspect:
 *    - cpu->halted          — exit requested?
 *    - cpu->in_critical     — critical region entered/exited?
 *    - cpu->block_reason    — should this VM be blocked? (defined
 *                             elsewhere; not vm_ecall's concern)
 *  and act accordingly. The router itself does NOT touch these.
 * ============================================================ */

void vm_ecall_dispatch(VmEcallRouter *r, VmCpu *cpu, void *system);

/* ============================================================
 *  Memory introspection record layouts (ABI)
 *
 *  Returned by SYS_SLAB_STATS and SYS_VM_STATS. All fields are
 *  little-endian on the wire (RV32 is LE, host is presumed LE).
 *  The version field on each struct allows future field
 *  additions; bumped only if existing field semantics change.
 * ============================================================ */

#define VM_SLAB_STATS_VERSION  1
#define VM_VM_STATS_VERSION    1

/* SLAB_BIN_COUNT shadow — must match memory/slab_stack.h.
 * Don't include slab_stack.h here (vm_ecall.h is the ABI header,
 * meant to be importable by guests with no host deps). Guests
 * see this as a fixed constant. */
#define VM_SLAB_STATS_BIN_COUNT  16

typedef struct {
    uint16_t version;            /* = VM_SLAB_STATS_VERSION */
    uint16_t bin_count;          /* = VM_SLAB_STATS_BIN_COUNT */

    /* Local slab — backs per-VM allocations. */
    uint32_t local_total;        /* slab.total_bytes_managed */
    uint32_t local_in_use;       /* slab.total_bytes_in_use */
    uint32_t local_peak;         /* slab.peak_bytes_in_use */
    uint32_t local_alloc_count;
    uint32_t local_free_count;
    uint32_t local_failed_count;

    /* Shared slab — backs SYS_ALLOC. */
    uint32_t shared_total;
    uint32_t shared_in_use;
    uint32_t shared_peak;
    uint32_t shared_alloc_count;
    uint32_t shared_free_count;
    uint32_t shared_failed_count;

    /* Per-bin info. Each entry: bits 0-15 = bucket_count, bits
     * 16-31 = blocks_in_use. block_size is implicit: bin i has
     * size (32 << i). */
    uint32_t local_bins[VM_SLAB_STATS_BIN_COUNT];
    uint32_t shared_bins[VM_SLAB_STATS_BIN_COUNT];
} VmSlabStatsRecord;

typedef struct {
    uint16_t version;            /* = VM_VM_STATS_VERSION */
    uint16_t vm_id;
    uint8_t  state;              /* 0=halted, 1=runnable, 2=blocked */
    uint8_t  in_critical;        /* 0/1 */
    uint8_t  alloc_count;        /* SYS_ALLOC blocks currently held */
    uint8_t  _pad;
    uint32_t text_bytes;         /* CODE region length */
    uint32_t rodata_bytes;       /* RODATA region length */
    uint32_t data_bytes;         /* DATA region length */
    uint32_t mailbox_bytes;      /* mailbox storage size */
    uint32_t instructions_retired_lo;
    uint32_t instructions_retired_hi;
    uint32_t trap_count;
    uint32_t ecall_count;
} VmVmStatsRecord;

#endif /* VM_ECALL_H */
