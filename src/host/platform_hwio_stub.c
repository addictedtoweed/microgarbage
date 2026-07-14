/* ============================================================
 *  platform_hwio_stub.c — REFERENCE hardware-I/O backend for a
 *  NEW target (companion to platform_stub.c)
 *
 *  A complete, compilable, *non-functional* implementation of
 *  host_hwio.h so a host that installs the hardware-I/O ecalls links
 *  from day one. Every function is __attribute__((weak)) and returns
 *  -VM_ENOSYS (readers return 0): a guest that pokes GPIO/I2C/SPI/
 *  ADC/PWM before the target implements them gets a clean "not
 *  implemented", never a crash.
 *
 *  Bring-up: build this file into your target, then provide NON-weak
 *  same-named functions (e.g. in platform_rpi.c poking BCM2835
 *  registers) one at a time; the linker prefers yours. See the weak
 *  caveats in platform_stub.c (signatures must match EXACTLY; MSVC
 *  has no portable weak — edit bodies instead).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/host_hwio.h"
#include "vm/vm_ecall.h"   /* VM_ENOSYS */

#if defined(__GNUC__) || defined(__clang__)
#  define HW_WEAK __attribute__((weak))
#else
#  define HW_WEAK   /* no weak support (e.g. MSVC): edit bodies */
#endif

/* ---- GPIO ---- */

HW_WEAK int host_hwio_gpio_config(uint32_t pin, uint32_t mode) {
    (void)pin; (void)mode;
    /* TODO(port): set the pin's function-select + pull. */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_gpio_write(uint32_t pin, uint32_t level) {
    (void)pin; (void)level;
    /* TODO(port): drive the output register (SET/CLR). */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_gpio_read(uint32_t pin) {
    (void)pin;
    /* TODO(port): sample the level register → 0/1. */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_gpio_toggle(uint32_t pin) {
    (void)pin;
    /* TODO(port): read-modify-write, or SET/CLR from cached state. */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_gpio_write_mask(uint32_t bank, uint32_t mask,
                                      uint32_t values) {
    (void)bank; (void)mask; (void)values;
    /* TODO(port): one SET + one CLR write covering `mask`. */
    return -VM_ENOSYS;
}

HW_WEAK uint32_t host_hwio_gpio_read_mask(uint32_t bank, uint32_t mask) {
    (void)bank; (void)mask;
    /* TODO(port): sample the bank's level register & mask. */
    return 0u;   /* reader idiom: no error channel */
}

/* ---- I2C ---- */

HW_WEAK int host_hwio_i2c_config(uint32_t bus, uint32_t hz) {
    (void)bus; (void)hz;
    /* TODO(port): set the bus divider for `hz`. */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_i2c_xfer(uint32_t bus, uint32_t addr,
                               const void *wbuf, uint32_t wlen,
                               void *rbuf, uint32_t rlen) {
    (void)bus; (void)addr; (void)wbuf; (void)wlen; (void)rbuf; (void)rlen;
    /* TODO(port): START; write wlen; repeated-START; read rlen; STOP. */
    return -VM_ENOSYS;
}

/* ---- SPI ---- */

HW_WEAK int host_hwio_spi_config(uint32_t bus, uint32_t mode, uint32_t hz) {
    (void)bus; (void)mode; (void)hz;
    /* TODO(port): set CPOL/CPHA + clock divider. */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_spi_xfer(uint32_t bus, uint32_t cs,
                               const void *tx, void *rx, uint32_t len) {
    (void)bus; (void)cs; (void)tx; (void)rx; (void)len;
    /* TODO(port): assert CS; shift len bytes full-duplex; deassert CS. */
    return -VM_ENOSYS;
}

/* ---- ADC ---- */

HW_WEAK int host_hwio_adc_read(uint32_t channel) {
    (void)channel;
    /* TODO(port): start a conversion, return the sample. The Pi Zero
     * has NO native ADC — leave this returning -VM_ENOSYS there. */
    return -VM_ENOSYS;
}

/* ---- PWM ---- */

HW_WEAK int host_hwio_pwm_config(uint32_t channel, uint32_t hz) {
    (void)channel; (void)hz;
    /* TODO(port): program the PWM period for `hz`. */
    return -VM_ENOSYS;
}

HW_WEAK int host_hwio_pwm_set(uint32_t channel, uint32_t duty_q16) {
    (void)channel; (void)duty_q16;
    /* TODO(port): program the compare/duty register. */
    return -VM_ENOSYS;
}
