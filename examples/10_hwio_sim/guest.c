/* 10_hwio_sim/guest.c — exercises the portable hardware-IO API.
 *
 * Uses examples/common/guest/hwio.h: configure a GPIO, drive/read it,
 * read a fake I2C temperature sensor (the canonical dev-kit case), poke
 * an absent I2C device to see the NAK, and read a fake ADC channel.
 *
 * Freestanding (no libc): output goes straight through SYS_WRITE, the
 * hardware calls through the SYS_GPIO / I2C / ADC ecalls. The SAME .elf
 * would run unchanged on the Pi dev kit against the real backend.
 *
 * Public domain (CC0). No warranty.
 */

#include "hwio.h"   /* pulls vm_runtime.h: SYS_*, _vm_sysN */

/* --- tiny freestanding console helpers over SYS_WRITE --- */
static void gw(const char *s, unsigned n) {
    _vm_sys3(SYS_WRITE, 1u, (uint32_t)(uintptr_t)s, n);
}
static void gs(const char *s) {
    unsigned n = 0; while (s[n]) n++; gw(s, n);
}
static void gi(int v) {
    char tmp[12]; int i = 0; unsigned u; int neg = 0;
    if (v < 0) { neg = 1; u = (unsigned)(-v); } else u = (unsigned)v;
    if (!u) tmp[i++] = '0';
    while (u) { tmp[i++] = (char)('0' + u % 10u); u /= 10u; }
    if (neg) tmp[i++] = '-';
    char out[12]; int j = 0;
    while (i) out[j++] = tmp[--i];
    gw(out, (unsigned)j);
}

static void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void _start(void) {
    gs("== hwio sim demo ==\n");

    /* GPIO: configure pin 17 as output, drive it, read it back, toggle. */
    hwio_gpio_config(17, HWIO_GPIO_OUT);
    hwio_gpio_write(17, 1);
    gs("gpio17         = "); gi(hwio_gpio_read(17)); gs("\n");
    hwio_gpio_toggle(17);
    gs("gpio17 toggled = "); gi(hwio_gpio_read(17)); gs("\n");

    /* I2C: read the LM75-style temperature sensor at 0x48, register 0. */
    hwio_i2c_config(1, 400000);
    unsigned char t[2] = { 0, 0 };
    int rc = hwio_i2c_read_reg(1, 0x48, 0x00, t, 2);
    if (rc == 0) {
        int whole = t[0];
        int half  = (t[1] & 0x80) ? 5 : 0;   /* .5 C in bit7 of low byte */
        gs("temp @0x48     = "); gi(whole); gs("."); gi(half); gs(" C\n");
    } else {
        gs("temp @0x48     : i2c error rc="); gi(rc); gs("\n");
    }

    /* I2C: an address with no device should NAK (-EIO). */
    rc = hwio_i2c_read_reg(1, 0x50, 0x00, t, 2);
    gs("i2c @0x50 absent : rc="); gi(rc); gs(" (expect nonzero)\n");

    /* ADC: fake channel read. */
    gs("adc ch0        = "); gi(hwio_adc_read(0)); gs("\n");

    /* Shared-region alloc round-trip — exercises the tier-1 ownership
     * map (SYS_ALLOC stamps the block's slots to this VM; the writes/
     * reads below flow through the shared-access check). 200 bytes
     * spans several 32-byte slots. */
    unsigned char *buf = (unsigned char *)(uintptr_t)_vm_sys1(SYS_ALLOC, 200);
    if (buf) {
        int ok = 1;
        for (int i = 0; i < 200; i++) buf[i] = (unsigned char)(i * 7 + 3);
        for (int i = 0; i < 200; i++)
            if (buf[i] != (unsigned char)(i * 7 + 3)) ok = 0;
        gs("shared alloc rw = "); gs(ok ? "ok" : "CORRUPT"); gs("\n");
        _vm_sys1(SYS_FREE, (uint32_t)(uintptr_t)buf);
    } else {
        gs("shared alloc rw = alloc failed\n");
    }

    gs("== done ==\n");
    sys_exit(0);
}
