/* ============================================================
 *  present.h — thin, swappable "video sink": put a framebuffer on screen.
 *
 *  The hardware-accelerated presentation seam for the PPU renderer. It
 *  knows NOTHING about the PPU — it just takes a finished RGBA8888
 *  framebuffer and draws it scaled + letterboxed to a window, vsync-
 *  paced. The PPU rasterizer fills the framebuffer; this shows it. That
 *  keeps the graphics-API dependency to one small file you can swap
 *  (OpenGL today; D3D11 later if you ever want post-FX shaders) without
 *  touching the emulator.
 *
 *  The first backend is OpenGL via WGL (src/video/present_gl_win32.c):
 *  flat C, no COM, hardware-accelerated, ships with Windows, CC0. Works
 *  from both the native and Cygwin hosts (a top-level Win32 window is
 *  independent of the terminal).
 *
 *  Pixel format: 32-bit, byte order R,G,B,A in memory. Build a pixel
 *  with PRESENT_RGBA(r,g,b); alpha is forced opaque and ignored on
 *  output. The PPU rasterizer produces this after BGR555 -> RGB8888.
 *
 *  Usage (once per frame):
 *      present_init(256, 224, "title");
 *      while (!present_should_close()) {
 *          ... fill framebuffer[256*224] ...
 *          present_frame(framebuffer);     // pumps input, draws, vsync-swaps
 *      }
 *      present_shutdown();
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VIDEO_PRESENT_H
#define VIDEO_PRESENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pack r,g,b (0..255) into the framebuffer pixel format (R,G,B,A bytes). */
#define PRESENT_RGBA(r, g, b) \
    ((uint32_t)(uint8_t)(r)            \
   | ((uint32_t)(uint8_t)(g) << 8)     \
   | ((uint32_t)(uint8_t)(b) << 16)    \
   | 0xFF000000u)

typedef enum {
    PRESENT_ASPECT_4_3 = 0,  /* letterbox to 4:3 NTSC display (default)   */
    PRESENT_ASPECT_SQUARE,   /* square pixels, integer scale (pixel-perfect) */
} PresentAspect;

typedef enum {
    PRESENT_FILTER_NEAREST = 0, /* crisp (default) */
    PRESENT_FILTER_LINEAR,      /* smooth          */
} PresentFilter;

/* Open a window for an fb_w x fb_h logical framebuffer. title may be
 * NULL. Returns false on failure (incl. platforms with no windowing).
 * Singleton: one window per process. */
bool present_init(int fb_w, int fb_h, const char *title);

/* Pump window messages, upload `framebuffer` (fb_w*fb_h pixels in the
 * format above), draw it scaled + letterboxed, and swap (vsync-paced).
 * Call once per presented frame. No-op if a close was requested. */
void present_frame(const uint32_t *framebuffer);

/* True once the user asked to close (window X or Esc). */
bool present_should_close(void);

/* Runtime toggles. Also bound to keys: F11 fullscreen, A aspect, F filter. */
void present_set_fullscreen(bool on);
void present_set_aspect(PresentAspect aspect);
void present_set_filter(PresentFilter filter);

void present_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PRESENT_H */
