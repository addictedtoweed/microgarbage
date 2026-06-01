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

/* -------- Frame budget introspection -------- */

/* Remaining DMA slots in this frame's 8-slot list. Decrements as
 * mg_chr_upload / mg_bg_upload / etc. queue. */
uint8_t  mg_frame_slots_remaining(void);

/* Remaining vblank byte budget. Tracks ~6479 bytes/frame baseline,
 * plus ~117 bytes/scanline of force-blank.  */
uint16_t mg_frame_bytes_remaining(void);

/* -------- Force-blank (letterbox for more DMA budget) -------- */

/* Set the force-blank window. `top` lines at the top of screen and
 * `bottom` lines at the bottom are blanked, trading visible area for
 * DMA budget. Each blanked scanline yields ~117 more bytes per frame.
 * Takes effect from the next frame. Default 0, 0.
 *
 * Common presets:
 *   (0, 0)   no forced blank — pure 224-line visible, baseline budget
 *   (8, 8)   "demo TV" 208 visible
 *   (16, 16) "movie" 192 visible — fits the FMV 30 fps 240x208 path
 */
void    mg_force_blank        (uint8_t top, uint8_t bottom);
uint8_t mg_force_blank_top    (void);
uint8_t mg_force_blank_bottom (void);

#ifdef __cplusplus
}
#endif

#endif /* MG_FRAME_H */
