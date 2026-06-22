/* ============================================================
 *  mg_fmv.h — guest-side wrapper for host-driven FMV playback.
 *
 *  The host FMV player owns the whole pipeline: it reads the .fmv,
 *  builds complete cart-ready frames ahead into a ring, and drives the
 *  SNES cart window from the FRAME_DONE consumer. The guest just kicks
 *  playback and waits:
 *
 *      if (mg_fmv_play("/host/movie.fmv") == 0) {
 *          while (mg_fmv_status() != MG_FMV_EOF) {
 *              MgPads p = mg_pads();
 *              if (mg_pad_pressed(p.p0, MG_BTN_START)) break;
 *              mg_wait_frame();
 *          }
 *          mg_fmv_stop();
 *      }
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_FMV_H
#define MG_FMV_H

#include "vm_runtime.h"   /* SYS_FMV_*, _vm_sys0/_vm_sys1 */
#include "fs.h"           /* fs_open, fs_close, O_RDONLY  */

/* Mirrors the host FMV_STATUS_* enum (fmv_player.h). */
enum {
    MG_FMV_IDLE    = 0,
    MG_FMV_PLAYING = 1,
    MG_FMV_EOF     = 2,
};

/* Start playback from an already-open fd. The host takes ownership of the
 * fd (closes it on stop). Returns 0 on success, negative on error. */
static inline int mg_fmv_play_fd(int fd) {
    return (int)_vm_sys1(SYS_FMV_PLAY, (uint32_t)fd);
}

/* Open `path` and start playback. On failure the fd (if opened) is closed.
 * Returns 0 on success, negative on error. */
static inline int mg_fmv_play(const char *path) {
    int fd = fs_open(path, O_RDONLY);
    if (fd < 0) return fd;
    int r = mg_fmv_play_fd(fd);
    if (r < 0) fs_close(fd);   /* host didn't take the fd */
    return r;
}

/* Stop playback and release host resources (incl. the fd). */
static inline void mg_fmv_stop(void) {
    (void)_vm_sys0(SYS_FMV_STOP);
}

/* MG_FMV_IDLE / MG_FMV_PLAYING / MG_FMV_EOF. */
static inline int mg_fmv_status(void) {
    return (int)_vm_sys0(SYS_FMV_STATUS);
}

/* Live-tune the per-scanline siphon force-blank H position (1..254). The host
 * applies it immediately + prints "fmv: siphon HTIME=N", so the D-pad can
 * sweep it in real time. */
static inline void mg_fmv_set_htime(int htime) {
    (void)_vm_sys1(SYS_FMV_SET_HTIME, (uint32_t)htime);
}

#endif /* MG_FMV_H */
