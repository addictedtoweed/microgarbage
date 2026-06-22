/* ============================================================
 *  fmv_player.h — FMV playback FSM + cart-window consumer.
 *
 *  Ties the FMV_VIDEO producer (fmv_video_stream) to the SNES cart
 *  window. The producer fills a ring of complete, cart-ready frames on
 *  the worker thread; this player consumes them on the bsnes thread at
 *  FRAME_DONE — a trivial pop → one DMA into the cart window, then walk
 *  the frame's 3 sub-frames. Because each promoted frame is already
 *  fully built, the just-in-time tight-burst pacing (the depth-2 shimmer)
 *  is gone structurally.
 *
 *  Threading:
 *    - Producer fill + start/stop/tick: worker thread.
 *    - try_kickoff + on_frame_done: bsnes thread (cart_window reads).
 *    The video ring is SPSC (worker produces, bsnes consumes). The cart
 *    window is bsnes-only once playing. s_active / s_state are atomics.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_FMV_PLAYER_H
#define MGAPI_FMV_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status, returned by fmv_player_status() / SYS_FMV_STATUS. */
enum {
    FMV_STATUS_IDLE    = 0,
    FMV_STATUS_PLAYING = 1,
    FMV_STATUS_EOF     = 2,
};

/* Begin playback from an already-open vm_host_fs fd, positioned at the
 * start of the file. Parses the FMV2 header, registers the FMV_VIDEO
 * stream, and arms the player. The player takes ownership of `fd` (closes
 * it at teardown). Returns false if already playing, the header is bad, or
 * registration fails (caller then still owns fd). Worker/ecall context. */
bool fmv_player_start(int fd);

/* Request stop. Stops the kernel bursting immediately; the actual
 * teardown (unregister/free/close) is deferred to the next worker tick so
 * no in-flight FRAME_DONE consumer touches freed state. Worker context. */
void fmv_player_stop(void);

/* FMV_STATUS_*. Cheap, lock-free. Any thread. */
int fmv_player_status(void);

/* Live siphon force-blank HTIME tune (1..254). Updates the cart-window config
 * immediately + logs the value, so a guest can sweep it in real time with the
 * D-pad. Worker/ecall context. */
void fmv_player_set_htime(uint8_t htime);

/* Hard teardown (mgapi_shutdown). Worker context, worker joined. */
void fmv_player_shutdown(void);

/* Worker-tick hook: runs the deferred teardown when a stop is pending.
 * Call from mgapi_step_body after stream_arbiter_tick. */
void fmv_player_tick(void);

/* True while an FMV owns the cart window — gates the cart_window.c
 * FRAME_DONE / FRAME_READY dispatch. bsnes context. */
bool fmv_player_active(void);

/* --- bsnes-thread cart-window hooks (called from cart_window.c) --- */

/* On the FRAME_READY read, the FMV player OWNS the frame-ready byte (it
 * overrides any stale non-FMV value left by the demo's clean_slate). Returns
 * the byte the kernel should see: 0 while pre-rolling or after EOF, 1 once a
 * frame is loaded and playing. Loads the first frame as a side effect once
 * the ring is primed. Call only when fmv_player_active(). */
uint8_t fmv_player_frame_ready_byte(void);

/* On the FRAME_DONE strobe: walk to the next sub-frame, or pop+promote the
 * next frame. Returns MG_ADVANCE_MORE / _PROMOTED / _EMPTY (see
 * copro_mg_state.h) so cart_window handles frame_ready/consumed uniformly. */
int fmv_player_on_frame_done(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MGAPI_FMV_PLAYER_H */
