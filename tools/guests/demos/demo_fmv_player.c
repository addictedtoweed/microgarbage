/* demo_fmv_player.c — FMV playback via the host FMV player (SYS_FMV_*).
 *
 * The entire staging pipeline now lives on the host: the FMV_VIDEO
 * producer reads /host/movie.fmv and builds complete, cart-window-ready
 * frames several frames ahead into a ring; the FRAME_DONE consumer pops
 * one and does a single DMA into the cart window. The guest's job shrinks
 * to: set up the kernel layout, kick playback, then wait. No per-frame
 * split / buffer-alternation / 5-chunk CHR dance, and — crucially — no
 * just-in-time staging, which is what made the depth-2 path shimmer.
 *
 * START exits cleanly; file-end auto-exits.
 *
 * Public domain (CC0). No warranty.
 */
#include "mg_input.h"
#include "mg_frame.h"
#include "mg_bg.h"
#include "mg_gfx.h"
#include "mg_fmv.h"
#include "vm_runtime.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}
static int sys_write(int fd, const void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_WRITE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static unsigned my_strlen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void dbg(const char *s) { sys_write(1, s, my_strlen(s)); }

void _start(void) {
    dbg("\r\nfmv_player: _start\r\n");

    /* Initialize the PPU shadow (zeroes VRAM so the BLANK_TILE margin tile
     * renders as backdrop) and flush that clear once before handing the cart
     * window to the FMV player. */
    mg_ppu_clean_slate();
    mg_frame_commit();
    mg_wait_frame();

    /* Full-height 15 fps: 8-line top + 8-line bottom letterbox. All 780 tiles
     * are delivered by the vblank burst across 4 sub-frames (no siphon) ->
     * pixel-clean full 240x208 at the native rate of the 15fps clip. */
    mg_kernel_layout(8, 8);

    if (mg_fmv_play("/host/movie.fmv") != 0) {
        dbg("fmv_player: play failed\r\n");
        sys_exit(1);
    }
    dbg("fmv_player: playing\r\n");

    /* Play until end-of-file; START exits early. */
    while (mg_fmv_status() != MG_FMV_EOF) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) break;
        mg_wait_frame();
    }

    mg_fmv_stop();
    dbg("fmv_player: done\r\n");
    sys_exit(0);
}
