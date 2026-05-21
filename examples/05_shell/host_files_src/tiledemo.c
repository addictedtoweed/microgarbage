/* tiledemo.c — demonstrates host-side tiles (round T.3b).
 *
 * Creates a few colored tiles, blits them in a pattern, sleeps,
 * exits. Validates that:
 *   - TILE_CREATE returns valid handles
 *   - TILE_FILL paints all cells
 *   - TILE_SET_TRANSPARENT marks individual cells
 *   - TILE_BLIT respects transparency
 *
 * Public domain (CC0).
 */

#include <stdint.h>

#define SYS_EXIT             93
#define SYS_SLEEP_TICKS    1045
#define SYS_TUI_INIT       1132
#define SYS_TUI_SHUTDOWN   1133
#define SYS_TUI_PRESENT    1135
#define SYS_TUI_FLUSH_DRAW 1138

#define SYS_TUI_TILE_CREATE          1139
#define SYS_TUI_TILE_FILL            1142
#define SYS_TUI_TILE_SET_TRANSPARENT 1143
#define SYS_TUI_TILE_BLIT            1144

#define OP_END         0
#define OP_FILL_RECT   2

#define TUI_USE_ALT_SCREEN  (1u << 0)
#define TUI_HIDE_CURSOR     (1u << 2)

/* Inline trampolines. */
static inline uint32_t sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys1(uint32_t n, uint32_t a) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys2(uint32_t n, uint32_t a, uint32_t b) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys3(uint32_t n, uint32_t a, uint32_t b, uint32_t c) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a2 asm("a2") = c;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys4(uint32_t n, uint32_t a, uint32_t b,
                             uint32_t c, uint32_t d) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a2 asm("a2") = c;
    register uint32_t a3 asm("a3") = d;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

void _start(void) {
    if ((int32_t)sys3(SYS_TUI_INIT, 10, 40,
                       TUI_USE_ALT_SCREEN | TUI_HIDE_CURSOR) < 0) {
        sys1(SYS_EXIT, 1);
    }

    /* Fill the canvas with '.' as background. */
    static uint8_t bg_buf[16];
    int o = 0;
    bg_buf[o++] = OP_FILL_RECT;
    bg_buf[o++] = 1; bg_buf[o++] = 0;     /* row=1 */
    bg_buf[o++] = 1; bg_buf[o++] = 0;     /* col=1 */
    bg_buf[o++] = 10; bg_buf[o++] = 0;    /* h=10 */
    bg_buf[o++] = 40; bg_buf[o++] = 0;    /* w=40 */
    bg_buf[o++] = '.';
    bg_buf[o++] = OP_END;
    sys2(SYS_TUI_FLUSH_DRAW, (uint32_t)(uintptr_t)bg_buf, (uint32_t)o);

    /* Create three 2x4 tiles with different colors. */
    uint32_t t_red    = sys2(SYS_TUI_TILE_CREATE, 2, 4);
    uint32_t t_green  = sys2(SYS_TUI_TILE_CREATE, 2, 4);
    uint32_t t_blue   = sys2(SYS_TUI_TILE_CREATE, 2, 4);

    /* TILE_FILL(handle, c<<8|attrs, fg, bg) — fill with '#' */
    uint32_t ca = ((uint32_t)'#' << 8);
    sys4(SYS_TUI_TILE_FILL, t_red,   ca, 1, 256);    /* fg=red */
    sys4(SYS_TUI_TILE_FILL, t_green, ca, 2, 256);    /* fg=green */
    sys4(SYS_TUI_TILE_FILL, t_blue,  ca, 4, 256);    /* fg=blue */

    /* Punch a transparent corner on the green tile. */
    sys3(SYS_TUI_TILE_SET_TRANSPARENT, t_green, 1, 1);
    sys3(SYS_TUI_TILE_SET_TRANSPARENT, t_green, 1, 2);

    /* Blit the three tiles in a row, with green's transparent
     * corner showing the '.' background through. */
    sys3(SYS_TUI_TILE_BLIT, t_red,   2, 4);
    sys3(SYS_TUI_TILE_BLIT, t_green, 2, 10);
    sys3(SYS_TUI_TILE_BLIT, t_blue,  2, 16);

    sys0(SYS_TUI_PRESENT);

    /* Sleep 1.5s. */
    sys1(SYS_SLEEP_TICKS, 1500);

    sys0(SYS_TUI_SHUTDOWN);
    sys1(SYS_EXIT, 0);
}
