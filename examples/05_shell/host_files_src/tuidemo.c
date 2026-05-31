/* tuidemo.c — proof-of-concept guest using the new host TUI service.
 *
 * Draws a 3x box with text inside, presents it, sleeps, and exits.
 * Uses SYS_TUI_INIT / SYS_TUI_FLUSH_DRAW / SYS_TUI_PRESENT /
 * SYS_TUI_SHUTDOWN directly via tiny inline syscall stubs — no
 * tui library linked.
 *
 * With the host-side TUI service doing the drawing, this is a
 * ~700 byte ELF (vs ~20 KB for an equivalent guest that linked
 * the old client-side TUI library).
 *
 * Public domain (CC0).
 */

#include <stdint.h>

/* Platform syscall numbers — match host's vm_ecall.h. */
#define SYS_EXIT             93
#define SYS_SLEEP_TICKS    1045
#define SYS_TUI_INIT       1132
#define SYS_TUI_SHUTDOWN   1133
#define SYS_TUI_PRESENT    1135
#define SYS_TUI_FLUSH_DRAW 1138

/* Draw-command opcodes. */
#define OP_END        0
#define OP_SET_CELL   1
#define OP_FILL_RECT  2
#define OP_PRINT      3
#define OP_BOX        4
#define OP_CLEAR      9

#define TUI_USE_ALT_SCREEN  (1u << 0)
#define TUI_HIDE_CURSOR     (1u << 2)

/* Inline syscall trampolines. */
static inline uint32_t sys3(uint32_t n, uint32_t a, uint32_t b, uint32_t c) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a2 asm("a2") = c;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline uint32_t sys2(uint32_t n, uint32_t a, uint32_t b) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline uint32_t sys1(uint32_t n, uint32_t a) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline uint32_t sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* Tiny u16 LE serializer for the command buffer. */
static int put_u16(uint8_t *b, int o, uint16_t v) {
    b[o++] = (uint8_t)(v & 0xff);
    b[o++] = (uint8_t)((v >> 8) & 0xff);
    return o;
}

void _start(void) {
    /* Initialize TUI: alt screen + cursor hidden, 10 rows × 40 cols. */
    if ((int32_t)sys3(SYS_TUI_INIT, 10, 40,
                       TUI_USE_ALT_SCREEN | TUI_HIDE_CURSOR) < 0) {
        sys1(SYS_EXIT, 1);
    }

    /* Build a small draw-command buffer.
     *
     * CLEAR — clear canvas
     * BOX(row=1, col=1, h=10, w=40, style=2 = ASCII)
     * PRINT(row=3, col=4, fg=7, bg=256, attrs=0, "hello from tuidemo")
     * PRINT(row=5, col=4, fg=2, bg=256, attrs=0, "this guest fits in ~700 bytes!")
     * END
     */
    static uint8_t buf[256];
    int o = 0;
    buf[o++] = OP_CLEAR;

    /* BOX */
    buf[o++] = OP_BOX;
    o = put_u16(buf, o, 1);   /* row */
    o = put_u16(buf, o, 1);   /* col */
    o = put_u16(buf, o, 10);  /* h */
    o = put_u16(buf, o, 40);  /* w */
    buf[o++] = 2;             /* ASCII style */

    /* PRINT line 1 */
    const char *msg1 = "hello from tuidemo";
    int m1len = 18;
    buf[o++] = OP_PRINT;
    o = put_u16(buf, o, 3);             /* row */
    o = put_u16(buf, o, 4);             /* col */
    o = put_u16(buf, o, 7);             /* fg=7 */
    o = put_u16(buf, o, 256);           /* bg=default */
    buf[o++] = 0;                       /* attrs */
    o = put_u16(buf, o, (uint16_t)m1len);
    for (int i = 0; i < m1len; i++) buf[o++] = (uint8_t)msg1[i];

    /* PRINT line 2 */
    const char *msg2 = "drawing via host syscalls!";
    int m2len = 26;
    buf[o++] = OP_PRINT;
    o = put_u16(buf, o, 5);
    o = put_u16(buf, o, 4);
    o = put_u16(buf, o, 2);             /* green */
    o = put_u16(buf, o, 256);
    buf[o++] = 0;
    o = put_u16(buf, o, (uint16_t)m2len);
    for (int i = 0; i < m2len; i++) buf[o++] = (uint8_t)msg2[i];

    buf[o++] = OP_END;

    /* Flush and present. */
    sys2(SYS_TUI_FLUSH_DRAW, (uint32_t)(uintptr_t)buf, (uint32_t)o);
    sys0(SYS_TUI_PRESENT);

    /* Sleep 1.5 seconds so the user can see the screen. */
    sys1(SYS_SLEEP_TICKS, 1500);

    sys0(SYS_TUI_SHUTDOWN);
    sys1(SYS_EXIT, 0);
}
