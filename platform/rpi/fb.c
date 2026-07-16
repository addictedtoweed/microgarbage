/* platform/rpi/fb.c — VideoCore mailbox framebuffer (first pixels).
 *
 * Requests a linear framebuffer from the GPU via the BCM2835 mailbox
 * property interface (channel 8), draws a test pattern, and can dump it
 * to a PPM (via semihosting) so the result is inspectable headless.
 *
 * The mailbox is the clean, "driverless" path to video the Pi port
 * leans on (docs/rpi-port.md): one tagged message sets the physical /
 * virtual size + depth, allocates the buffer, and reads back the pitch.
 *
 * Coherency note (real HW; QEMU has no cache model so it's a no-op
 * there): the message buffer is cleaned from the D-cache before the GPU
 * reads it, and the response is invalidated before we read it. On real
 * hardware the framebuffer itself also wants non-cacheable mapping or a
 * cache clean after drawing so the GPU sees the pixels — left for the
 * hardware bring-up; here we verify via CPU readback + PPM, which is
 * coherent with our own cached writes.
 *
 * Public domain (CC0). No warranty.
 */

#include <stdint.h>
#include <string.h>

#define PERIPH_BASE   0x20000000u
#define MBOX_BASE     (PERIPH_BASE + 0x00B880u)
#define MBOX_READ     (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS   (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE    (*(volatile uint32_t *)(MBOX_BASE + 0x20))
#define MBOX_FULL     0x80000000u
#define MBOX_EMPTY    0x40000000u
#define MBOX_CH_PROP  8u

/* framebuffer state, filled by fb_request() */
uint32_t fb_base, fb_size, fb_pitch, fb_w, fb_h, fb_bpp;

/* ---- cache maintenance (real-HW coherency; no-op under QEMU) ---- */
static void dcache_clean_all(void) {
    __asm__ volatile(
        "mcr p15, 0, r0, c7, c10, 0\n"   /* clean entire D-cache      */
        "mcr p15, 0, r0, c7, c10, 4\n"   /* DSB                       */
        ::: "r0", "memory");
}
static void dcache_invalidate_all(void) {
    __asm__ volatile(
        "mcr p15, 0, r0, c7, c6, 0\n"    /* invalidate entire D-cache */
        "mcr p15, 0, r0, c7, c10, 4\n"   /* DSB                       */
        ::: "r0", "memory");
}

static void mbox_write(uint32_t chan, uint32_t data28) {
    while (MBOX_STATUS & MBOX_FULL) { }
    MBOX_WRITE = (data28 & ~0xFu) | (chan & 0xFu);
}
static uint32_t mbox_read(uint32_t chan) {
    for (;;) {
        while (MBOX_STATUS & MBOX_EMPTY) { }
        uint32_t d = MBOX_READ;
        if ((d & 0xFu) == chan) return d & ~0xFu;
    }
}

/* One tagged property message: set physical + virtual W/H + depth +
 * pixel order, allocate the buffer, read the pitch. Returns 0 on success. */
int fb_request(uint32_t width, uint32_t height, uint32_t bpp) {
    static volatile uint32_t __attribute__((aligned(16))) mbox[36];
    int i = 0, ai, pi;
    mbox[i++] = 0;                 /* size (patched below)              */
    mbox[i++] = 0;                 /* request                           */
    mbox[i++] = 0x48003; mbox[i++] = 8; mbox[i++] = 8; mbox[i++] = width;  mbox[i++] = height; /* phys W/H */
    mbox[i++] = 0x48004; mbox[i++] = 8; mbox[i++] = 8; mbox[i++] = width;  mbox[i++] = height; /* virt W/H */
    mbox[i++] = 0x48005; mbox[i++] = 4; mbox[i++] = 4; mbox[i++] = bpp;                        /* depth    */
    mbox[i++] = 0x48006; mbox[i++] = 4; mbox[i++] = 4; mbox[i++] = 1;                          /* order RGB*/
    ai = i; mbox[i++] = 0x40001; mbox[i++] = 8; mbox[i++] = 8; mbox[i++] = 16; mbox[i++] = 0;  /* alloc: align->base,size */
    pi = i; mbox[i++] = 0x40008; mbox[i++] = 4; mbox[i++] = 4; mbox[i++] = 0;                  /* get pitch */
    mbox[i++] = 0;                 /* end tag                           */
    mbox[0] = (uint32_t)(i * 4);

    dcache_clean_all();
    mbox_write(MBOX_CH_PROP, (uint32_t)(uintptr_t)mbox);
    (void)mbox_read(MBOX_CH_PROP);
    dcache_invalidate_all();

    if (mbox[1] != 0x80000000u) return -1;         /* request rejected */
    fb_base  = mbox[ai + 3] & 0x3FFFFFFFu;          /* mask GPU alias -> ARM phys */
    fb_size  = mbox[ai + 4];
    fb_pitch = mbox[pi + 3];
    fb_w = width; fb_h = height; fb_bpp = bpp;
    if (!fb_base || !fb_pitch) return -1;
    return 0;
}

static inline volatile uint32_t *fb_row(uint32_t y) {
    return (volatile uint32_t *)(uintptr_t)(fb_base + y * fb_pitch);
}

/* 8 classic vertical color bars. */
void fb_draw_bars(void) {
    static const uint32_t bar[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000
    };
    for (uint32_t y = 0; y < fb_h; y++) {
        volatile uint32_t *row = fb_row(y);
        for (uint32_t x = 0; x < fb_w; x++)
            row[x] = bar[(x * 8u) / fb_w];
    }
}

uint32_t fb_pixel(uint32_t x, uint32_t y) { return fb_row(y)[x]; }

/* ---- semihosting PPM dump (headless visual proof) ----
 * _write is routed to the UART, so we call semihosting directly for the
 * host-side file. */
#define SH_OPEN 0x01
#define SH_CLOSE 0x02
#define SH_WRITE 0x05
static int semihost(int op, const void *block) {
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = block;
    __asm__ volatile("svc #0x123456" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static int sh_open(const char *name, int mode) {
    uint32_t a[3] = { (uint32_t)(uintptr_t)name, (uint32_t)mode, (uint32_t)strlen(name) };
    return semihost(SH_OPEN, a);
}
static int sh_write(int fh, const void *data, uint32_t len) {
    uint32_t a[3] = { (uint32_t)fh, (uint32_t)(uintptr_t)data, len };
    return semihost(SH_WRITE, a);   /* returns bytes NOT written (0 = ok) */
}
static void sh_close(int fh) { uint32_t a = (uint32_t)fh; semihost(SH_CLOSE, &a); }

/* Write the framebuffer to a binary PPM (P6). Returns 0 on success. */
int fb_dump_ppm(const char *filename) {
    static uint8_t rowbuf[4096 * 3];
    char hdr[64];
    int n = 0;
    const char *p = "P6\n";
    while (*p) hdr[n++] = *p++;
    /* "<w> <h>\n255\n" — tiny hand-rolled itoa */
    uint32_t vals[2] = { fb_w, fb_h };
    for (int k = 0; k < 2; k++) {
        char tmp[10]; int t = 0; uint32_t v = vals[k];
        if (!v) tmp[t++] = '0';
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) hdr[n++] = tmp[--t];
        hdr[n++] = (k == 0) ? ' ' : '\n';
    }
    p = "255\n"; while (*p) hdr[n++] = *p++;

    int fh = sh_open(filename, 5 /* "wb" */);
    if (fh < 0) return -1;
    sh_write(fh, hdr, (uint32_t)n);
    for (uint32_t y = 0; y < fb_h; y++) {
        volatile uint32_t *row = fb_row(y);
        for (uint32_t x = 0; x < fb_w && x < 4096; x++) {
            uint32_t px = row[x];
            rowbuf[x * 3 + 0] = (uint8_t)(px >> 16);   /* R */
            rowbuf[x * 3 + 1] = (uint8_t)(px >> 8);    /* G */
            rowbuf[x * 3 + 2] = (uint8_t)(px);         /* B */
        }
        sh_write(fh, rowbuf, fb_w * 3u);
    }
    sh_close(fh);
    return 0;
}
