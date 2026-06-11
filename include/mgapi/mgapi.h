/* ============================================================
 *  mgapi.h — the cart-side runtime, exposed as a stable C ABI.
 *
 *  Two embeddings consume this header:
 *
 *    1. mgapi.dll on Windows. A custom bsnes-plus mapper LoadLibrary's
 *       it, GetProcAddress's every mgapi_* symbol, and forwards SNES
 *       cart-bus reads + per-frame joypad state through it. The DLL
 *       owns the full microgarbage runtime (VM + audio + trashfs +
 *       shell over TCP) behind the seam.
 *
 *    2. libmgapi.a on the STM32 H745 M7. Same source, statically
 *       linked into the firmware. The 64 KB DTCM IS the cart window;
 *       a UART transports the shell instead of a TCP listener.
 *
 *  The bsnes mapper sees ONLY this header. Everything else
 *  under `src/mgapi/` is implementation detail.
 *
 *  See `docs/snes-coprocessor-game.md` (TODO) for the cart-bus
 *  contract this header pairs with, and `snes/copro.inc` for the
 *  exact addresses the SNES kernel side hits.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_H
#define MGAPI_H

#include <stdint.h>

#if defined(_WIN32) && !defined(MGAPI_STATIC)
#  if defined(MGAPI_BUILDING_DLL)
#    define MGAPI_API __declspec(dllexport)
#  else
#    define MGAPI_API __declspec(dllimport)
#  endif
#else
#  define MGAPI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 *  Configuration
 * ---------------------------------------------------------------- */

/* The cart window the SNES side sees. Always 64 KB on the real
 * hardware (HiROM, full bank mirrored). The mapper must call
 * mgapi_init with this exact size; supplying anything else is a
 * configuration error and mgapi_init returns -EINVAL.
 */
#define MGAPI_CART_WINDOW_BYTES   (64u * 1024u)

/* Sample rate the audio engine produces. Hardcoded to match what
 * the existing audio_service is built around. Mapper must consume
 * pulled samples at this rate (resample on its side if its audio
 * output differs).
 */
#define MGAPI_AUDIO_SAMPLE_RATE_HZ  44100u

/* Output format from mgapi_audio_pull: interleaved stereo int16.
 * 4 bytes per frame.
 */
#define MGAPI_AUDIO_BYTES_PER_FRAME 4u

/* Values for MgapiConfig::rom_select. */
#define MGAPI_ROM_SMOKE 0u
#define MGAPI_ROM_BOOT  1u
#define MGAPI_ROM_NONE  2u

/* Reset behavior knobs. The SNES /RESET line is held asserted for at
 * least `hold_ms` after every mgapi_cart_reset_begin call. This
 * window covers the CIC lockout chip's ~20 ms hold AND lets the
 * cart-window restage complete cleanly before the SNES starts
 * fetching $FFFC again. mgapi_cart_reset_ready returns true once
 * the elapsed time has reached `hold_ms`.
 *
 * 0 in MgapiConfig::reset.hold_ms selects the default (50 ms).
 */
typedef struct MgapiResetConfig {
    uint32_t hold_ms;
} MgapiResetConfig;

typedef struct MgapiConfig {
    /* Cart window size; must equal MGAPI_CART_WINDOW_BYTES. */
    uint32_t  cart_window_size;

    /* Sample rate the mgapi mixer AND host audio sink will run at.
     *
     *   0           — auto-detect. On Windows this queries WASAPI's
     *                 default render endpoint mix-format rate (what
     *                 Audio Engine actually plays the device at).
     *                 Recommended for emulator-style hosts where the
     *                 OS owns audio configuration. MCU builds MUST
     *                 NOT pass 0 (there is no device to ask).
     *   non-zero    — taken as-is. MCU firmware passes the I2S clock
     *                 rate it wired up (typically MGAPI_AUDIO_SAMPLE_
     *                 RATE_HZ = 44100 for PCM5100-class DACs);
     *                 lower-power MCUs can configure 32000 or 22050
     *                 to trade audio bandwidth for CPU headroom.
     *
     * The mixer's stream-resampling path handles source WAVs at any
     * rate, so source assets don't need to match. */
    uint32_t  audio_sample_rate;

    /* Upper bound on the chunk size the mapper will request from
     * mgapi_audio_pull in any one call. The ring buffer between
     * mgapi_step and mgapi_audio_pull is sized as a small multiple
     * of this; under-provisioning causes audible underruns.
     * Reasonable default: 4096 (~93 ms at 44.1 kHz).
     */
    uint32_t  audio_frames_max;

    /* TCP port the shell listens on. 0 disables (no TCP). PuTTY
     * connects in Raw or Telnet mode to localhost:port. The bsnes
     * mapper would typically pass 2323 here so the user can attach
     * a terminal while the emulator runs.
     */
    uint16_t  tcp_listen_port;

    /* Number of joypads the SNES side will poll. The kernel always
     * reads all four mailbox slots; this just controls how many
     * the mapper bothers populating. 2 is the conventional case.
     */
    uint8_t   pad_count;

    /* Cart ROM auto-load policy:
     *   0 (MGAPI_ROM_SMOKE)  = load the all-in-one smoke ROM
     *                          (boot.s + smoke.s with hardcoded menu;
     *                          runs visibly under bsnes-plus with no
     *                          copro guest needed — good for first
     *                          integration tests)
     *   1 (MGAPI_ROM_BOOT)   = load the runtime kernel (boot.s +
     *                          kernel.s); the menu / game logic lives
     *                          in a copro guest ELF that drives the
     *                          cart window via SYS_COPRO_*
     *   2 (MGAPI_ROM_NONE)   = leave the window zero; the embedder
     *                          stages via mgapi_dev_load_blob or a
     *                          guest does it via SYS_COPRO_STAGE_PAYLOAD
     *
     * If the chosen ROM wasn't baked in at build time, the loader
     * silently falls through to "none" — call mgapi_dev_pool_sizes
     * or mgapi_version after init to verify what's actually loaded.
     */
    uint8_t   rom_select;

    /* Path to the shell ELF. NULL = use the runtime's baked-in copy.
     * Used by tooling that wants a non-default shell; the mapper
     * normally passes NULL.
     */
    const char *shell_elf_path;

    /* Path to a file (in trashfs `/td0/` or host `/host/`) the shell
     * runs at startup. NULL = no autostart; shell drops to prompt.
     * The shell continues running after the autostart ELF exits.
     */
    const char *autostart_path;

    /* Reset timing. Zero-filled gives the default 50 ms hold. */
    MgapiResetConfig reset;

    /* When non-zero: don't install a default process-stdio transport
     * for the shell VM. Set this when running embedded inside an
     * emulator (bsnes-plus) or MCU firmware — the embedder's main
     * thread shouldn't be stuck inside vm_system_step when the shell
     * does sys_read(stdin) and the underlying Windows console read
     * blocks. With this flag set:
     *   - shell still loads, but reads from stdin return EOF;
     *   - shell exits cleanly on its first read attempt;
     *   - TCP transport still binds per-VM on accept (PuTTY works);
     *   - audio / VM / cart-window state all continue to advance.
     */
    uint8_t  disable_default_stdio;
    uint8_t  _reserved_pad[7];   /* future flags; must be zero */
} MgapiConfig;

/* ----------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------- */

/* Initialize the runtime. Allocates the 8 MB audio pool, starts the
 * VM scheduler (with the shell as the first VM), and opens the TCP
 * listener if configured. Safe to call once; calling again before
 * mgapi_shutdown returns -EALREADY.
 *
 * Returns 0 on success, negative errno on failure:
 *   -EINVAL : bad config (wrong window size, etc.)
 *   -ENOMEM : audio pool / VM state alloc failed
 *   -EADDRINUSE : TCP port already bound by another process
 *   -ENOSYS : not implemented (during early bring-up of the DLL)
 */
MGAPI_API int  mgapi_init(const MgapiConfig *cfg);

/* Tear down. Closes the TCP listener (disconnects any active PuTTY
 * sessions), halts every VM, frees the audio pool. Safe to call even
 * if mgapi_init failed. After this returns, mgapi_init may be called
 * again with a fresh config.
 */
MGAPI_API void mgapi_shutdown(void);

/* ----------------------------------------------------------------
 *  Cart-bus seam
 * ---------------------------------------------------------------- */

/* Serve one cart-bus read. `addr` is the full SNES 24-bit address
 * (bank << 16 | offset16). The HiROM mirror is applied internally:
 * only the low 16 bits select the byte. Side effects:
 *
 *   - reads in $7000-$77FF latch the joypad mailbox index decoded
 *     from the high address bits;
 *   - a read of $7E00 transitions the status byte from "boot ready"
 *     to "runtime" (one-shot — repeated reads do nothing further);
 *   - reads of $7800 expose the current frame-ready byte;
 *   - reads of $7808-$7847 expose the DMA descriptor list;
 *   - reads of $7F00 expose the status byte;
 *   - all other addresses read the static window contents (the boot
 *     blob in $8000-$FFFF, the per-frame payload in $0000-$7DFF).
 *
 * Returns the byte the cart bus should yield. Never blocks.
 */
MGAPI_API uint8_t mgapi_cart_read(uint32_t addr);

/* Post the latest joypad state. The mapper calls this once per
 * emulated frame, after polling whichever input device its host
 * uses. Word format matches the SNES auto-joypad layout (the bits
 * at $4218 / $4219):
 *
 *     bit 15..4 = B Y Select Start Up Down Left Right A X L R
 *     bit  3..0 = controller-type signature (0 for std pad)
 *
 * pads[0..pad_count-1] are read; the rest are ignored. The values
 * are latched and served to the SNES kernel on its next mailbox
 * read (see the JOYPORT_P*_LO/HI ports in copro.inc).
 */
MGAPI_API void mgapi_post_joypads(const uint16_t pads[4]);

/* ----------------------------------------------------------------
 *  Per-frame tick + audio pull
 * ---------------------------------------------------------------- */

/* Advance the runtime. The mapper calls this once per emulated
 * frame (or whenever it can — the runtime catches up). elapsed_ns
 * is the wall-clock time since the last call, used to drive the
 * VM scheduler's tick source and to size how many audio frames to
 * generate into the ring.
 *
 * If the mapper passes zero elapsed_ns, the runtime advances by a
 * fixed default quantum — useful for bring-up and testing.
 */
MGAPI_API void mgapi_step(uint64_t elapsed_ns);

/* Drain rendered audio frames into the mapper's buffer. Returns
 * the number of frames actually written (may be less than `frames`
 * on underrun — typical first call after init, or if mgapi_step has
 * been starved). The mapper handles underrun by repeating its last
 * sample or zero-filling.
 *
 * dst_stereo is interleaved L,R,L,R,... int16, 4 bytes per frame.
 */
MGAPI_API uint32_t mgapi_audio_pull(int16_t *dst_stereo, uint32_t frames);

/* ----------------------------------------------------------------
 *  Diagnostics
 * ---------------------------------------------------------------- */

/* Returns a static string like "mgapi 0.1 (commit abc1234)". The
 * mapper logs this on load so a tester can pin which DLL build is
 * actually being talked to. Always non-NULL.
 */
MGAPI_API const char *mgapi_version(void);

/* ----------------------------------------------------------------
 *  Cart reset marshalling
 * ----------------------------------------------------------------
 *
 * The embedder calls these around its emulated reset to model the
 * real cart's reset hold (CIC chip + window-stabilization). The
 * sequence on the bsnes side is:
 *
 *     mgapi_cart_reset_begin();             // restage window, snapshot timer
 *     while (!mgapi_cart_reset_ready()) {
 *         mgapi_step(1000000);              // let copro guest run
 *     }
 *     // bsnes proceeds with its own emulated reset
 *     mgapi_cart_reset_end();
 *
 * The copro guest sees a monotonically-incrementing reset counter
 * via SYS_COPRO_RESET_COUNT (mg_copro_reset_count). Guests that
 * care reload state when the counter changes; guests that don't
 * just keep staging frames as usual — the kernel re-boots from the
 * restaged window and walks whatever DMA list is there. */

/* Mark the cart bus as held in reset; restage the static portion
 * of the window from the chosen ROM; reset status/frame_ready;
 * bump the reset counter visible to guests; capture the timer
 * start. Returns immediately. */
MGAPI_API void mgapi_cart_reset_begin(void);

/* True once `hold_ms` has elapsed since the last reset_begin.
 * 1 = ready to release; 0 = still held. Cheap to poll. */
MGAPI_API int mgapi_cart_reset_ready(void);

/* Acknowledge that the embedder has released its reset. No-op
 * today; reserved for telemetry / state-machine hooks later. */
MGAPI_API void mgapi_cart_reset_end(void);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_H */
