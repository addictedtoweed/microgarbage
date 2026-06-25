/* ============================================================
 *  mgapi_init.c — bring the runtime up and tear it down.
 *
 *  STAGE 1 SCAFFOLD: wires only the cart-window seam through to
 *  the public ABI. No audio pool, no VM, no TCP — those land in
 *  separate, additive commits so we can verify the
 *  LoadLibrary->cart_read path before pulling the whole runtime
 *  in.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#define MGAPI_BUILDING_DLL 1
#include "mgapi/mgapi.h"

#include "cart_window.h"
#include "fmv_player.h"
#include "psram_pool.h"
#include "audio_init.h"
#include "cart_volume.h"
#include "l2_init.h"
#include "vm_init.h"
#include "tcp_listen.h"
#include "worker.h"

#include "io/stream_arbiter.h"
#include "storage/trashfs.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull `errno.h` codes into negative-return form. mingw and MSVC
 * both expose these via <errno.h>, so we don't need shims here. */

static int          g_initialized = 0;
static PsramLayout  g_psram;
static uint32_t     g_audio_frames_max;   /* config-side hint for the pump */

/* Reset bookkeeping. The selected ROM is remembered so reset_begin
 * can re-stage the same image cold-boot did. hold_ms is the timer-
 * based release threshold; reset_t_start_ms is the wall-clock stamp
 * captured by reset_begin. */
static uint8_t      g_rom_select;
static uint32_t     g_reset_hold_ms = 50;
static uint32_t     g_reset_t_start_ms;

extern uint32_t host_platform_monotonic_ms(void *userdata);

/* Forward decl for the per-vblank tick body. Defined further down,
 * but mgapi_init hands its address to the worker thread at startup. */
static void mgapi_step_body(uint64_t elapsed_ns);

/* ----------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------- */

int mgapi_init(const MgapiConfig *cfg) {
    if (g_initialized) return -EALREADY;
    if (!cfg) return -EINVAL;
    if (cfg->cart_window_size != MGAPI_CART_WINDOW_BYTES) return -EINVAL;
    /* v1.94: audio_sample_rate == 0 means "auto-detect" (Windows queries
     * WASAPI; MCU build falls back to 44100). Any non-zero value is
     * taken as-is — embedders that want a specific rate (MCU twin
     * builds for a fixed I2S clock, host harnesses pinning a known
     * rate) just pass it. The old equality check on
     * MGAPI_AUDIO_SAMPLE_RATE_HZ was hostile to platforms where 44100
     * isn't the right answer; the constant remains the MCU default. */
    if (cfg->audio_sample_rate != 0 && cfg->audio_sample_rate < 8000) {
        return -EINVAL;
    }
    if (cfg->audio_frames_max == 0) return -EINVAL;
    if (cfg->pad_count > 4) return -EINVAL;
    if (cfg->rom_select > MGAPI_ROM_NONE) return -EINVAL;
    for (size_t i = 0; i < sizeof cfg->_reserved_pad; i++) {
        if (cfg->_reserved_pad[i] != 0) return -EINVAL;
    }

    cart_window_init();

    /* Pick the effective ROM selection. The embedder's cfg.rom_select
     * is the default, but $MGAPI_ROM_SELECT overrides it -- handy when
     * the embedder (e.g. the bsnes-plus cart class) was compiled with
     * one value and the user wants a different one without rebuilding
     * the host. Accepts "smoke" / "boot" / "none"; anything else is
     * ignored and we fall through to the embedder's value. */
    uint8_t rom_select = cfg->rom_select;
    {
        const char *env = getenv("MGAPI_ROM_SELECT");
        if (env) {
            if      (strcmp(env, "smoke") == 0) rom_select = MGAPI_ROM_SMOKE;
            else if (strcmp(env, "boot")  == 0) rom_select = MGAPI_ROM_BOOT;
            else if (strcmp(env, "none")  == 0) rom_select = MGAPI_ROM_NONE;
            fprintf(stderr, "mgapi: MGAPI_ROM_SELECT=%s -> rom_select=%u\n",
                    env, (unsigned)rom_select);
            fflush(stderr);
        }
    }

    /* Remember the ROM selection so reset_begin can re-stage from the
     * same source on warm reset. Also resolve hold_ms (0 = default). */
    g_rom_select    = rom_select;
    g_reset_hold_ms = cfg->reset.hold_ms ? cfg->reset.hold_ms : 50u;

    /* Auto-load the chosen ROM image into the cart window. See
     * MgapiConfig::rom_select. Falls through silently if the picked
     * ROM wasn't baked in. */
    extern const unsigned char smoke_rom[];
    extern const size_t        smoke_rom_len;
    extern const unsigned char boot_rom[];
    extern const size_t        boot_rom_len;
    if (rom_select == MGAPI_ROM_SMOKE && smoke_rom_len > 0) {
        cart_window_load_blob(0, smoke_rom, (uint32_t)smoke_rom_len);
    } else if (rom_select == MGAPI_ROM_BOOT && boot_rom_len > 0) {
        cart_window_load_blob(0, boot_rom, (uint32_t)boot_rom_len);
    }
    /* MGAPI_ROM_NONE: leave the window zero — caller will stage. */

    /* Stage 2a: claim the 8 MB PSRAM block + carve it three ways.
     * The cart-trashfs / audio / L2 slices are passed to their
     * respective subsystem inits in subsequent stages. */
    int rc = psram_pool_init(&g_psram);
    if (rc != 0) {
        cart_window_shutdown();
        return rc;
    }

    /* Stage 2b1: bring up the AudioService on the 4 MB audio slice.
     * Stage 2b1 only renders silence — no clients are connected yet,
     * the mixer just runs idle. The ring path is the thing under
     * test; channel + service are wired so stage 3's VM lands on a
     * working seam. */
    rc = mgapi_audio_init(g_psram.audio, g_psram.audio_size,
                          cfg->audio_sample_rate);
    if (rc != 0) {
        psram_pool_shutdown();
        cart_window_shutdown();
        return rc;
    }
    g_audio_frames_max = cfg->audio_frames_max;

    /* Stage 2c: format + mount the /cart/ trashfs volume on the
     * 1 MB cart slice. Registration with the VM fs mount table
     * happens in stage 3 once the VM is up. */
    rc = mgapi_cart_volume_init(g_psram.cart_trashfs,
                                g_psram.cart_trashfs_size);
    if (rc != 0) {
        mgapi_audio_shutdown();
        psram_pool_shutdown();
        cart_window_shutdown();
        return rc;
    }

    /* Stage 2d: bring up the L2 allocator on the 3 MB slice. Stage 3
     * will carve this into per-VM regions and instantiate one
     * L2Alloc per VM; today we run a single shared allocator that
     * the host test exercises directly. */
    rc = mgapi_l2_init(g_psram.l2, g_psram.l2_size);
    if (rc != 0) {
        mgapi_cart_volume_shutdown();
        mgapi_audio_shutdown();
        psram_pool_shutdown();
        cart_window_shutdown();
        return rc;
    }

    /* Stage 3a: VM system + embedded shell ELF. The cart volume
     * handle (a TrashfsVolume*) is mounted as /cart/; the shell ELF
     * is loaded and starts running on the first mgapi_step. The L2
     * backing is the 3 MB PSRAM slice — guests reach it via VAs at
     * 0xE000_0000+, with the VM translating to this region. The L2
     * allocator (mgapi_l2_init) owns the carve inside this slice. */
    mgapi_vm_set_l2_backing(g_psram.l2, g_psram.l2_size);
    mgapi_vm_set_disable_default_stdio(cfg->disable_default_stdio);
    mgapi_vm_set_autostart_path(cfg->autostart_path);   /* .sfc MGBOOT tag, or NULL */
    rc = mgapi_vm_init(mgapi_cart_volume_handle());
    if (rc != 0) {
        mgapi_l2_shutdown();
        mgapi_cart_volume_shutdown();
        mgapi_audio_shutdown();
        psram_pool_shutdown();
        cart_window_shutdown();
        return rc;
    }

    /* Stage 4: TCP listener for PuTTY. Opens the listen socket but
     * doesn't accept until the first mgapi_step poll. Failure to
     * bind (port already in use, etc.) is non-fatal — the runtime
     * stays up, the shell just isn't reachable over TCP. */
    if (cfg->tcp_listen_port != 0) {
        uint16_t shell_id = mgapi_vm_shell_id();
        if (shell_id != 0xFFFFu) {
            int trc = mgapi_tcp_listen_init(cfg->tcp_listen_port, shell_id);
            if (trc != 0) {
                /* Non-fatal — the rest of the runtime keeps running. */
            }
        }
    }

    /* tcp_listen_port / shell_elf_path / autostart_path are read
     * but not yet acted on — we accept the config so the mapper
     * can pass its real values today and the future stages light
     * up without an ABI bump. */
    (void)cfg->tcp_listen_port;
    (void)cfg->shell_elf_path;
    (void)cfg->autostart_path;

    g_initialized = 1;

    /* Stage 5 (v1.68): hand the per-vblank tick body off to the
     * worker thread. Embedder's mgapi_step now just signals the
     * worker; everything that used to run synchronously on the
     * bsnes-plus thread (VM, audio mixing, TCP poll, diag) now
     * runs concurrently on this dedicated thread. Started LAST so
     * the worker only runs against a fully-initialized runtime. */
    if (mgapi_worker_start(mgapi_step_body) != 0) {
        /* Non-fatal: without the worker, mgapi_step is a no-op and
         * nothing advances — but the embedder can still poll the
         * cart window. Log and continue so a CI host running
         * without thread support can still build-test. */
        fprintf(stderr, "mgapi_init: worker_start failed; runtime will not "
                        "advance VM/audio until restarted\n");
        fflush(stderr);
    }

    return 0;
}

void mgapi_shutdown(void) {
    if (!g_initialized) return;
    /* Stop the worker FIRST so the tick body never sees a half-
     * torn-down VM/audio/TCP. After stop() returns the worker
     * thread has joined and will not touch any of the subsystems
     * we're about to shut down. */
    mgapi_worker_stop();
    /* Worker is joined; tear the FMV player down first (it unregisters its
     * FMV_VIDEO stream from the arbiter and closes its fd). */
    fmv_player_shutdown();
    /* Worker is joined; safe to tear down the stream arbiter (it has
     * no producer thread of its own — the worker tick was the only
     * writer). Closes any still-registered streams + frees ring
     * storage. */
    stream_arbiter_shutdown();
    mgapi_tcp_listen_shutdown();
    mgapi_vm_shutdown();
    mgapi_l2_shutdown();
    mgapi_cart_volume_shutdown();
    mgapi_audio_shutdown();
    psram_pool_shutdown();
    memset(&g_psram, 0, sizeof g_psram);
    cart_window_shutdown();
    g_initialized = 0;
}

/* ----------------------------------------------------------------
 *  Cart-bus seam (pure forwarders to cart_window)
 * ---------------------------------------------------------------- */

uint8_t mgapi_cart_read(uint32_t addr) {
    if (!g_initialized) return 0;
    return cart_window_read(addr);
}

void mgapi_post_joypads(const uint16_t pads[4]) {
    if (!g_initialized || !pads) return;
    cart_window_post_pads(pads);
}

void mgapi_post_mouse(int dx, int dy, unsigned buttons) {
    if (!g_initialized) return;
    cart_window_post_mouse(dx, dy, (uint8_t)buttons);
}

/* ----------------------------------------------------------------
 *  Diagnostic: cart-bus access tracking
 *
 *  When the SNES side stays solid black despite a guest staging
 *  frames, the question is always "is the SNES actually running our
 *  kernel?" Easiest way to tell: instrument cart_window_read so we
 *  can see what offsets the SNES is fetching. mgapi_diag_periodic
 *  (called from mgapi_step) prints a one-line summary roughly every
 *  second.
 *
 *  Lifecycle markers we surface immediately:
 *    BOOT_STROBE    boot.s reached its end, jumped to kernel.s
 *    FIRST $7800    kernel's NMI handler ran for the first time
 *    FIRST DMA-LIST kernel reached its DMA dispatch (proves NMI is
 *                   not just looping at the gate)
 * ---------------------------------------------------------------- */

static uint32_t g_diag_cart_reads;
static uint16_t g_diag_last_off;
static bool     g_diag_boot_strobed;
static bool     g_diag_nmi_seen;
static bool     g_diag_dma_seen;
static uint64_t g_diag_last_print_ns;
static uint64_t g_diag_clock_ns;

void mgapi_diag_note_cart_read(uint16_t off, uint32_t full) {
    (void)full;
    g_diag_cart_reads++;
    g_diag_last_off = off;
    if (off == CW_OFF_STROBE_BOOT && !g_diag_boot_strobed) {
        g_diag_boot_strobed = true;
        fprintf(stderr, "mgapi: BOOT_STROBED -- boot.s reached kernel jump\n");
        fflush(stderr);
    }
    if (off == 0x7800 /* FRAME_READY */ && !g_diag_nmi_seen) {
        g_diag_nmi_seen = true;
        fprintf(stderr, "mgapi: FIRST $7800 read -- kernel NMI fired\n");
        fflush(stderr);
    }
    if (off >= 0x7808 && off < 0x7848 && !g_diag_dma_seen) {
        g_diag_dma_seen = true;
        fprintf(stderr,
                "mgapi: FIRST DMA-LIST read at $%04X -- kernel walking slots\n",
                (unsigned)off);
        fflush(stderr);
    }
}

static void mgapi_diag_periodic(uint64_t elapsed_ns) {
    g_diag_clock_ns += elapsed_ns;
    /* v1.85: gated behind MGAPI_DIAG=1. The per-second cart_reads /
     * ppu_batch / slot listing was useful during bring-up but the
     * fprintf traffic was holding up the worker thread enough to
     * cause audible FFT/audio skips. Set MGAPI_DIAG=1 in the env
     * if you need to see this again (e.g., a new demo not rendering).
     * One env-var read per second is negligible vs the I/O it saves. */
    static int s_diag_enabled = -1;   /* -1 = unread, 0 = off, 1 = on */
    if (s_diag_enabled == -1) {
        const char *e = getenv("MGAPI_DIAG");
        s_diag_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!s_diag_enabled) return;
    /* One-line status per ~1 second of wall-clock so the user can see
     * if the SNES is actually executing without spamming the log. */
    if (g_diag_clock_ns - g_diag_last_print_ns < 1000000000ull) return;
    g_diag_last_print_ns = g_diag_clock_ns;
    fprintf(stderr,
            "mgapi diag: cart_reads=%u last_off=$%04X boot=%d nmi=%d dma=%d\n",
            g_diag_cart_reads, (unsigned)g_diag_last_off,
            g_diag_boot_strobed ? 1 : 0,
            g_diag_nmi_seen     ? 1 : 0,
            g_diag_dma_seen     ? 1 : 0);
    /* Dump what the SNES sees in the staging area: PPU batch first
     * 12 bytes (BGMODE..MOSAIC), then DMA slots 0..2. That tells us
     * whether the host is staging plausible PPU register values and
     * DMA descriptors -- if a slot's bbus is $00 or its src points
     * outside payload, the kernel will skip or read garbage. */
    extern uint8_t mgapi_cart_read(uint32_t addr);
    fprintf(stderr,
            "  ppu_batch: bgmode=%02X obsel=%02X bg1sc=%02X bg12nba=%02X tm=%02X ts=%02X\n",
            mgapi_cart_read(0xC07848), mgapi_cart_read(0xC07849),
            mgapi_cart_read(0xC0784A), mgapi_cart_read(0xC0784E),
            mgapi_cart_read(0xC07850), mgapi_cart_read(0xC07851));
    for (int s = 0; s < 8; s++) {
        uint32_t base = 0xC07808 + s * 8;
        uint8_t bbus = mgapi_cart_read(base + 0);
        uint8_t dmap = mgapi_cart_read(base + 1);
        uint16_t src = mgapi_cart_read(base + 2) | (mgapi_cart_read(base + 3) << 8);
        uint16_t sz  = mgapi_cart_read(base + 4) | (mgapi_cart_read(base + 5) << 8);
        uint16_t prep= mgapi_cart_read(base + 6) | (mgapi_cart_read(base + 7) << 8);
        if (bbus == 0 && sz == 0) continue;
        fprintf(stderr,
                "  slot[%d]: bbus=$%02X dmap=$%02X src=$%04X size=%u prep=$%04X\n",
                s, bbus, dmap, src, sz, prep);
    }
    /* INIDISP HDMA table at $7868. Expected for fullscreen (no
     * force-blank) with our emit_inidisp_table generator:
     *     FF 0F E1 0F 00 ...      (5 bytes: 127 lines + 97 lines)
     * If the data byte after a count byte is $80, force-blank is
     * being written per scanline -> screen blanks. If the table
     * looks truncated (00 too early) HDMA stops and INIDISP keeps
     * whatever value it had before, which boot.s set to $0F. */
    fprintf(stderr, "  inidisp_tbl: ");
    for (int i = 0; i < 16; i++) {
        fprintf(stderr, "%02X ", mgapi_cart_read(0xC07868 + i));
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ----------------------------------------------------------------
 *  Per-frame tick + audio pull (stubbed for stage 1)
 * ---------------------------------------------------------------- */

/* Per-vblank tick body. Runs on the worker thread (see worker.c).
 * The embedder's mgapi_step just kicks the worker; everything that
 * touches the VM, audio service, or TCP listener lives in here so
 * the bsnes-plus thread is never stalled by ecall I/O or mixing. */
static void mgapi_step_body(uint64_t elapsed_ns) {
    if (!g_initialized) return;

    /* Convert elapsed wall-clock to a frame count at the audio rate.
     * 1e9 ns / 44100 Hz ≈ 22675 ns per frame; we round down (so we
     * never overshoot the ring). elapsed_ns == 0 means "embedder
     * doesn't track elapsed yet" — render one safe quantum so the
     * ring stays alive for bring-up testing. */
    uint32_t frames;
    if (elapsed_ns == 0) {
        frames = 256;   /* ~5.8 ms — small enough to not overflow on
                         * back-to-back calls; big enough to be useful */
    } else {
        uint64_t f = elapsed_ns / 22675ull;
        /* Clamp to the config's max-chunk hint so we never render an
         * unbounded burst even if the embedder hands us a huge dt. */
        if (g_audio_frames_max > 0 && f > g_audio_frames_max) {
            f = g_audio_frames_max;
        }
        frames = (uint32_t)f;
    }
    mgapi_audio_pump(frames);

    /* Diagnostic heartbeat -- one stderr line per second showing cart
     * bus activity so we can tell "kernel is running" apart from
     * "kernel never started." */
    mgapi_diag_periodic(elapsed_ns ? elapsed_ns : 16666667ull);

    /* Stage 4: poll the TCP listener for new client connections.
     * Cheap when no listener is configured (early-out inside). */
    mgapi_tcp_listen_poll();

    /* v2.10: pump the stream arbiter BEFORE the VM step. Producer-
     * first ordering: any guest about to call SYS_STREAM_CONSUME
     * inside vm_step finds its ring already topped up this tick,
     * so it returns a chunk immediately instead of having to
     * sleep-and-retry. Cheap when no streams are registered (the
     * for-loop inside walks STREAM_ARBITER_MAX empty slots and
     * exits). The arbiter and vm_step share a thread; any
     * critical section inside the vm_step that would block
     * doesn't delay this read because reads already happened. */
    (void)stream_arbiter_tick();

    /* FMV player: runs the deferred teardown when a stop is pending. Its
     * producer rides the stream_arbiter_tick above; its consumer is the
     * bsnes-thread FRAME_DONE path. */
    fmv_player_tick();

    /* Stage 3a: drive the cooperative scheduler. We step until it
     * goes idle (no ready VMs / all blocked) or runs out of budget.
     * v2.35: cap raised 64 -> 4096. The loop already exits early when
     * mgapi_vm_step() returns false (idle/blocked), so in normal play
     * this just lets a guest stage a WHOLE frame in one tick instead of
     * being truncated mid-staging and forced to wait for the next signal
     * (which, combined with the wake-on-frame-consumed fix, is what gets
     * FMV from ~11 fps back to a full 15). 4096 is a runaway backstop. */
    for (int i = 0; i < 4096; i++) {
        if (!mgapi_vm_step()) break;
    }
}

/* Embedder-facing kick. The actual per-vblank work happens on the
 * worker thread (see worker.c + mgapi_step_body above). This is
 * deliberately sub-microsecond: an atomic store + atomic add +
 * SetEvent. The bsnes-plus thread never blocks on VM/audio/TCP. */
void mgapi_step(uint64_t elapsed_ns) {
    if (!g_initialized) return;
    mgapi_worker_signal(elapsed_ns);
}

uint32_t mgapi_audio_pull(int16_t *dst_stereo, uint32_t frames) {
    if (!g_initialized) return 0;
    /* #73: the embedder calls this once per SNES output sample, so it is the
     * emulator's SNES master-clock tick — publish it for FMV A/V drift sync. */
    mgapi_audio_note_snes_clock(frames);
    return mgapi_audio_drain(dst_stereo, frames);
}

/* ----------------------------------------------------------------
 *  Diagnostics
 * ---------------------------------------------------------------- */

const char *mgapi_version(void) {
    return "mgapi 2.30.14 (NMI re-arms HIRQ even when frame_ready=0 — fixes whole-screen-black flicker)";
}

/* ----------------------------------------------------------------
 *  Cart reset
 * ---------------------------------------------------------------- */

void mgapi_cart_reset_begin(void) {
    if (!g_initialized) return;

    /* Pick the same ROM cold-boot used. NONE = restage zeros. */
    extern const unsigned char smoke_rom[];
    extern const size_t        smoke_rom_len;
    extern const unsigned char boot_rom[];
    extern const size_t        boot_rom_len;
    const void *rom = NULL;
    uint32_t    n   = 0;
    if (g_rom_select == MGAPI_ROM_SMOKE && smoke_rom_len > 0) {
        rom = smoke_rom; n = (uint32_t)smoke_rom_len;
    } else if (g_rom_select == MGAPI_ROM_BOOT && boot_rom_len > 0) {
        rom = boot_rom;  n = (uint32_t)boot_rom_len;
    }
    cart_window_reset_begin(rom, n);
    g_reset_t_start_ms = host_platform_monotonic_ms(NULL);
}

int mgapi_cart_reset_ready(void) {
    if (!g_initialized) return 1;
    uint32_t now     = host_platform_monotonic_ms(NULL);
    uint32_t elapsed = now - g_reset_t_start_ms;
    return elapsed >= g_reset_hold_ms ? 1 : 0;
}

void mgapi_cart_reset_end(void) {
    /* Reserved for telemetry / state hooks. No-op today. */
}

/* ----------------------------------------------------------------
 *  Stage 1 extension: let the test harness drop a smoke-ROM blob
 *  into the window so cart reads expose the SNES boot stub. NOT
 *  part of the public mgapi.h ABI — this is exported only for the
 *  in-tree mgapi_host_test exerciser. The real bsnes mapper goes
 *  through SYS_COPRO_STAGE_PAYLOAD (stage 3) or via the embedded
 *  boot blob (stage 3 too).
 * ---------------------------------------------------------------- */

MGAPI_API void mgapi_dev_load_blob(uint32_t offset,
                                   const void *src,
                                   uint32_t len) {
    if (!g_initialized) return;
    cart_window_load_blob(offset, src, len);
}

/* Dev: hand the test harness the PSRAM carve so it can assert sizes
 * and (with later stages) that the three subregions don't overlap.
 * Sizes are uint32_t — fine, the largest slice fits comfortably. */
MGAPI_API void mgapi_dev_pool_sizes(uint32_t out[3]) {
    if (!g_initialized || !out) {
        if (out) { out[0] = out[1] = out[2] = 0; }
        return;
    }
    out[0] = (uint32_t)g_psram.cart_trashfs_size;
    out[1] = (uint32_t)g_psram.audio_size;
    out[2] = (uint32_t)g_psram.l2_size;
}

/* Dev: ring fill / capacity, so the host test can prove the pump
 * actually rendered something and the drain reduced the fill. */
MGAPI_API void mgapi_dev_audio_ring(uint32_t out[2]) {
    if (!out) return;
    out[0] = g_initialized ? mgapi_audio_ring_used()     : 0;
    out[1] = g_initialized ? mgapi_audio_ring_capacity() : 0;
}

/* Dev: stats from the /cart/ trashfs volume. The host test uses
 * these to confirm format+mount produced a sensible volume on the
 * 1 MB region. out = {total_blocks, free_blocks, inode_count,
 * free_inodes}. Returns 0 on success, -EAGAIN if not mounted. */
MGAPI_API int mgapi_dev_cart_stats(uint32_t out[4]) {
    if (!out) return -EINVAL;
    CartVolumeStats s;
    int r = mgapi_cart_volume_stats(&s);
    if (r != 0) { out[0] = out[1] = out[2] = out[3] = 0; return r; }
    out[0] = s.total_blocks;
    out[1] = s.free_blocks;
    out[2] = s.inode_count;
    out[3] = s.free_inodes;
    return 0;
}

/* Dev: direct access to the host-side L2 allocator. Bypasses the
 * (future stage 3) per-VM carve + VA<->PA translation so the host
 * test can exercise alloc/free/coalesce semantics in isolation. */
MGAPI_API void *mgapi_dev_l2_alloc(uint32_t size, uint32_t align) {
    return mgapi_l2_host_alloc(size, align);
}
MGAPI_API void  mgapi_dev_l2_free(void *p) { mgapi_l2_host_free(p); }

/* Dev: load a guest ELF and run it to completion. Used by the host
 * test to verify guest-side L2 round-trip without an interactive
 * shell. Returns the guest's exit code or a negative errno. */
extern const unsigned char l2_test_elf[];
extern const size_t        l2_test_elf_len;

MGAPI_API int mgapi_dev_run_l2_test(void) {
    if (l2_test_elf_len == 0) return -ENOENT;
    return mgapi_dev_spawn_elf_and_wait(l2_test_elf,
                                        (uint32_t)l2_test_elf_len);
}

/* Dev: spawn one of the bundled demo ELFs by name and run it for a
 * fixed number of steps, then halt + unload. Used by the host test
 * to verify the demos actually load + execute through the same
 * loader path the shell uses — distinguishing "load failure" from
 * "shell path-resolve failure." Returns the spawn entry code (0 on
 * load OK, negative on load failure) so the test can report it. */
extern const unsigned char demo_palette_elf  [];
extern const size_t        demo_palette_elf_len;
extern const unsigned char demo_sprite_elf   [];
extern const size_t        demo_sprite_elf_len;
extern const unsigned char demo_mode7_elf    [];
extern const size_t        demo_mode7_elf_len;
extern const unsigned char demo_mode7_3d_elf [];
extern const size_t        demo_mode7_3d_elf_len;

/* Dev: confirm the bundled demo is installed in /td0/demos/<name>.elf
 * and return its size in bytes. Returns -ENOENT if the file isn't
 * there. Used by the host test to distinguish "demos baked in but
 * not installed to /td0/" from "demos missing entirely." */
MGAPI_API int mgapi_dev_td0_demo_size(const char *name) {
    extern TrashfsVolume *mgapi_vm_td0_volume(void);
    TrashfsVolume *vol = mgapi_vm_td0_volume();
    if (!vol || !name) return -EINVAL;
    char path[64];
    snprintf(path, sizeof path, "/demos/%s.elf", name);
    TrashfsFile f;
    if (trashfs_open(vol, path, 0 /*RDONLY*/, &f) != TRASHFS_OK)
        return -ENOENT;
    int sz = (int)f.size;
    trashfs_close(&f);
    return sz;
}

/* Dev: slurp /td0/demos/<name>.elf via trashfs, then spawn-and-wait
 * with the resulting bytes. Mirrors slurp_file + spawn in
 * vm_host_fs_spawn.c — exposes whether the file's bytes load OK as
 * a separate dev step (so we can tell a trashfs_read truncation
 * apart from a path-resolve bug apart from a loader bug). Returns
 *   >  0   load OK + finished N steps (n bytes slurped)
 *   == 0   load OK
 *   < 0    error (-errno: ENOENT, EIO, EINVAL, ENOMEM) */
MGAPI_API int mgapi_dev_spawn_demo_via_trashfs(const char *name) {
    extern TrashfsVolume *mgapi_vm_td0_volume(void);
    TrashfsVolume *vol = mgapi_vm_td0_volume();
    if (!vol || !name) return -EINVAL;
    char path[64];
    snprintf(path, sizeof path, "/demos/%s.elf", name);
    TrashfsFile f;
    if (trashfs_open(vol, path, 0, &f) != TRASHFS_OK) return -ENOENT;
    size_t sz = f.size;
    uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
    if (!buf) { trashfs_close(&f); return -ENOMEM; }
    uint32_t got = 0;
    TrashfsResult tr = trashfs_read(&f, buf, (uint32_t)sz, &got);
    trashfs_close(&f);
    if (tr != TRASHFS_OK || got != sz) {
        free(buf);
        return -EIO;
    }
    int rc = mgapi_dev_spawn_elf_for_steps(buf, (uint32_t)sz, 5000);
    free(buf);
    return rc;
}

MGAPI_API int mgapi_dev_spawn_demo_for_steps(const char *name, uint32_t steps) {
    const unsigned char *bytes = NULL;
    size_t                len  = 0;
    if (!name) return -EINVAL;
    if      (strcmp(name, "palette")   == 0) { bytes = demo_palette_elf;   len = demo_palette_elf_len; }
    else if (strcmp(name, "sprite")    == 0) { bytes = demo_sprite_elf;    len = demo_sprite_elf_len; }
    else if (strcmp(name, "mode7")     == 0) { bytes = demo_mode7_elf;     len = demo_mode7_elf_len; }
    else if (strcmp(name, "mode7_3d")  == 0) { bytes = demo_mode7_3d_elf;  len = demo_mode7_3d_elf_len; }
    else return -ENOENT;
    if (len == 0) return -ENOENT;
    return mgapi_dev_spawn_elf_for_steps(bytes, (uint32_t)len, steps);
}

/* Dev: spawn menu.elf, run it for enough scheduler steps to stage
 * one frame, then halt + unload. The cart window state stays valid
 * after this returns — the host test reads it via mgapi_cart_read
 * to verify the staged CGRAM / tilemap / DMA list match what the
 * SNES kernel expects.
 *
 * Returns 0 on success, -ENOENT if menu.elf wasn't baked in,
 * -EAGAIN if the VM system isn't up. */
extern const unsigned char menu_elf[];
extern const size_t        menu_elf_len;

MGAPI_API int mgapi_dev_stage_menu_one_frame(void) {
    if (!g_initialized) return -EAGAIN;
    if (menu_elf_len == 0) return -ENOENT;
    return mgapi_dev_spawn_elf_for_steps(menu_elf, (uint32_t)menu_elf_len,
                                         100000u);
}

/* Dev: VM system + shell-load status, for the host test to confirm
 * stage 3a wiring. out = {has_shell_elf, shell_elf_bytes,
 * vm_system_alive, shell_loaded, shell_vm_id}. */
MGAPI_API void mgapi_dev_vm_stats(uint32_t out[5]) {
    if (!out) return;
    MgapiVmStats s;
    mgapi_vm_stats(&s);
    out[0] = s.has_shell_elf ? 1u : 0u;
    out[1] = (uint32_t)s.shell_elf_bytes;
    out[2] = s.vm_system_alive ? 1u : 0u;
    out[3] = s.shell_loaded ? 1u : 0u;
    out[4] = (uint32_t)s.shell_vm_id;
}

/* Dev: L2 stats — used / free bytes, alloc / free-block counts,
 * largest single-allocation possible. */
MGAPI_API int mgapi_dev_l2_stats(uint64_t out[5]) {
    if (!out) return -EINVAL;
    MgapiL2Stats s;
    int r = mgapi_l2_stats(&s);
    if (r != 0) {
        for (int i = 0; i < 5; i++) out[i] = 0;
        return r;
    }
    out[0] = (uint64_t)s.used_bytes;
    out[1] = (uint64_t)s.free_bytes;
    out[2] = (uint64_t)s.alloc_count;
    out[3] = (uint64_t)s.free_block_count;
    out[4] = (uint64_t)s.largest_free_bytes;
    return 0;
}
