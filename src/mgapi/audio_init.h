/* ============================================================
 *  audio_init.h — mgapi's audio subsystem owner.
 *
 *  Internal to mgapi.dll / libmgapi: stands up the AudioService on
 *  the PSRAM audio slice, owns the channel the service drains, and
 *  owns the small stereo ring buffer between mgapi_step (which
 *  pumps the mixer into the ring) and mgapi_audio_pull (which
 *  drains the ring for the embedder's audio output).
 *
 *  Stage 2b1: silence pump only — no clients of the channel exist
 *  yet (the VM lands in stage 3, file-reader in stage 3+). The
 *  mixer runs and produces silence; the ring path is verified by
 *  the host test.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_AUDIO_INIT_H
#define MGAPI_AUDIO_INIT_H

#include "vm/vm_system.h"
#include "audio/audio_ring_stream.h"   /* AudioRingStream (FMV clip audio, #73) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the service up on the given pool region. region/size are the
 * `audio` slice handed back by psram_pool_init.
 *
 * `requested_rate_hz` is the host's preferred mixer + sink rate.
 * Pass 0 to auto-detect: on Windows we query the default WASAPI render
 * endpoint's mix format; on the MCU there is no device to ask and the
 * caller MUST pass a concrete rate (typically 44100 for PCM5100-class
 * DACs). Any non-zero value is taken as-is — the mixer renders at
 * that rate and the audio sink opens at the same rate, eliminating
 * any post-mixer resampling on the way out.
 *
 * Returns 0 / -errno.
 */
int  mgapi_audio_init(void *pool_region, size_t pool_region_size,
                      uint32_t requested_rate_hz);

/* Tear down: stop the service, destroy the channel. Safe to call
 * even if init failed. */
void mgapi_audio_shutdown(void);

/* Install SYS_AUDIO_* ecall handlers on the given VmSystem and
 * register /host_fs_root/ as the resolution root for "/host/foo.wav"
 * style paths. Call AFTER mgapi_audio_init and AFTER the VmSystem
 * is up. Returns true on success. Without this call, mg_sfx_load /
 * mg_stream_play / audio_get_levels all return -ENOSYS and silently
 * no-op on the guest side. */
bool mgapi_audio_install_ecalls(VmSystem *sys,
                                const char *host_fs_root);

/* Advance the audio engine by `frames` mixed stereo frames, pushed
 * into the ring buffer. If the ring is full this clamps internally
 * (a slow embedder draining late). Also drains pending channel
 * requests (no-op in stage 2b1, but kept here so stage 3's VM-side
 * clients land on a working path).
 */
void mgapi_audio_pump(uint32_t frames);

/* Drain up to `frames` frames from the ring into dst_stereo
 * (interleaved L,R,L,R int16). Returns the actual frame count
 * written; less than `frames` means underrun and the embedder
 * zero-fills (or repeats) the tail. */
uint32_t mgapi_audio_drain(int16_t *dst_stereo, uint32_t frames);

/* Dev: snapshot of ring fill level (frames), for the host test. */
uint32_t mgapi_audio_ring_used(void);
uint32_t mgapi_audio_ring_capacity(void);

/* #73: the audio service's FMV clip-audio ring (NULL if audio isn't up). The
 * FMV video producer pushes the clip's muxed audio into it; the FMV music voice
 * (opened via vm_host_audio_fmv_open) drains it. */
AudioRingStream *mgapi_audio_fmv_ring(void);

/* FMV spectrum overlay: refcounted enable of the FFT band meter, and a band
 * read (copies up to `max` levels 0..255, returns count). No-op/0 if audio is
 * down. The meter runs over the final mixed output, so it tracks the movie's
 * own audio. */
void     mgapi_audio_fft_hold(bool on);
uint32_t mgapi_audio_fft_read(uint8_t *out, uint32_t max);

/* #73: feed the SNES master clock to FMV A/V drift sync. Call once per SNES
 * output sample from the embedder's audio-output cadence (mgapi_audio_pull),
 * passing the frame count pulled. No-op until audio is up. */
void mgapi_audio_note_snes_clock(uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_AUDIO_INIT_H */
