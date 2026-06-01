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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the service up on the given pool region. region/size are the
 * `audio` slice handed back by psram_pool_init. Returns 0 / -errno.
 */
int  mgapi_audio_init(void *pool_region, size_t pool_region_size);

/* Tear down: stop the service, destroy the channel. Safe to call
 * even if init failed. */
void mgapi_audio_shutdown(void);

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

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_AUDIO_INIT_H */
