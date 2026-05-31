/* ============================================================
 *  mg_frame.h — frame pacing for cart-side games.
 *
 *  The runtime's hot path owns per-frame DMA dispatch, joypad poll,
 *  mailbox read, and publishes a frame-ready producer. Guests block
 *  on the producer via mg_wait_frame; on return, pad state is fresh
 *  and staging tables are clear for the next frame's writes.
 *  mg_frame_commit is non-blocking; the runtime picks up staging
 *  tables at its next DMA-prep tick.
 *
 *  See docs/game-api.md for the full pacing model.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_FRAME_H
#define MG_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Block until the runtime publishes the next frame. On return,
 * mg_pads() reflects pads sampled this frame and the previous
 * frame's staged OAM/CHR/palette have been DMA'd out. */
void mg_wait_frame(void);

/* Mark this frame's staging tables ready for the next vblank.
 * Non-blocking. Calling mg_wait_frame without an intervening commit
 * is legal — the runtime sees no changes and repeats. */
void mg_frame_commit(void);

#ifdef __cplusplus
}
#endif

#endif /* MG_FRAME_H */
