/* ============================================================
 *  fmv_video_stream.h — FMV_VIDEO stream producer.
 *
 *  A StreamProducer (see io/stream_arbiter.h) that reads FMV2 video
 *  blocks from a file fd and decodes each into a complete, cart-window-
 *  ready MgCompleteFrame in the arbiter's ring — several frames ahead of
 *  the SNES consumer. This relocates demo_fmv.c's per-frame "staging
 *  dance" (split, A/B alternation, palette pairing, tilemap splat,
 *  5-chunk CHR, PPU-batch flip) onto the host, so frames are built ahead
 *  and the FRAME_DONE consumer is a trivial pop → DMA. That removes the
 *  just-in-time tight-burst pacing that caused the depth-2 shimmer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_FMV_VIDEO_STREAM_H
#define MGAPI_FMV_VIDEO_STREAM_H

#include <stdint.h>

#include "io/stream_arbiter.h"
#include "dma_engine.h"
#include "audio/audio_ring_stream.h"   /* AudioRingStream — FMV clip audio (#73) */

#ifdef __cplusplus
extern "C" {
#endif

/* FMV2 video block size: CGRAM(256) + raw tilemap(1560) + CHR(24960). */
#define FMV_VIDEO_BLOCK_BYTES  26776u

/* Register an FMV_VIDEO stream with the arbiter. `fd` must be positioned
 * just past the 32-byte FMV2 header (the player parses the header first).
 * `abytes` = audio bytes per unit (int16 stereo). `audio_ring` (may be NULL) is
 * the FMV clip-audio ring — the producer pushes each unit's leading audio chunk
 * into it as it produces, and the FMV music voice drains it in sync. `nframes`
 * from the header. `dma` is the transfer engine (HOST→PSRAM frame fills).
 * `depth` = ring depth, power of two ≥ 2 (use 4 for ~3 frames of lookahead).
 * Returns a StreamHandle or STREAM_HANDLE_INVALID; caller still owns fd/dma. */
StreamHandle fmv_video_stream_open(int fd, uint32_t abytes, uint32_t nframes,
                                   MgDmaEngine *dma, uint32_t depth,
                                   AudioRingStream *audio_ring);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MGAPI_FMV_VIDEO_STREAM_H */
