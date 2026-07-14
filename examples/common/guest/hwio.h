/* ============================================================
 *  hwio.h — guest hardware-I/O API (GPIO / I2C / SPI / ADC / PWM)
 *
 *  Thin, freestanding wrappers over the SYS_GPIO_* / SYS_I2C_* /
 *  SYS_SPI_* / SYS_ADC_* / SYS_PWM_* ecalls (1250..1279). The host
 *  drives the real pins; the guest issues portable transactions, so
 *  the SAME .elf runs on the Pi dev kit and on the deployment micro —
 *  only the host shim changes. See docs/rpi-port.md.
 *
 *  These are TRANSACTIONS, not per-edge bit-banging: one call does a
 *  whole I2C/SPI transfer. Timing-critical protocols live in the host
 *  shim, not here.
 *
 *  Return convention: >= 0 on success, or a NEGATIVE errno on failure
 *  (e.g. -38 == -ENOSYS when the target has no such peripheral). The
 *  mask/read "reader" calls return the sampled value directly.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GUEST_HWIO_H
#define GUEST_HWIO_H

#include "vm_runtime.h"   /* SYS_*, _vm_sysN */
#include <stdint.h>

/* GPIO pin modes — keep in sync with HWIO_GPIO_* in
 * include/vm/host_hwio.h. */
#define HWIO_GPIO_IN            0u
#define HWIO_GPIO_OUT           1u
#define HWIO_GPIO_IN_PULLUP     2u
#define HWIO_GPIO_IN_PULLDOWN   3u
#define HWIO_GPIO_OUT_OPENDRAIN 4u

/* ---- GPIO ---- */

/* Configure a pin (HWIO_GPIO_*). Returns 0 or -errno. */
static inline int hwio_gpio_config(unsigned pin, unsigned mode) {
    return (int)_vm_sys2(SYS_GPIO_CONFIG, pin, mode);
}

/* Drive an output pin (level 0/1). Returns 0 or -errno. */
static inline int hwio_gpio_write(unsigned pin, unsigned level) {
    return (int)_vm_sys2(SYS_GPIO_WRITE, pin, level ? 1u : 0u);
}

/* Sample an input pin. Returns 0 or 1, or -errno. */
static inline int hwio_gpio_read(unsigned pin) {
    return (int)_vm_sys1(SYS_GPIO_READ, pin);
}

/* Flip an output pin. Returns 0 or -errno. */
static inline int hwio_gpio_toggle(unsigned pin) {
    return (int)_vm_sys1(SYS_GPIO_TOGGLE, pin);
}

/* Batched write: set the `mask` pins of `bank` to `values` in one
 * transaction. Returns 0 or -errno. */
static inline int hwio_gpio_write_mask(unsigned bank, uint32_t mask,
                                       uint32_t values) {
    return (int)_vm_sys3(SYS_GPIO_WRITE_MASK, bank, mask, values);
}

/* Batched read: sample the `mask` pins of `bank`. Returns the
 * sampled bits (reader idiom — an absent bank reads 0). */
static inline uint32_t hwio_gpio_read_mask(unsigned bank, uint32_t mask) {
    return _vm_sys2(SYS_GPIO_READ_MASK, bank, mask);
}

/* ---- I2C ---- */

/* Set an I2C bus clock (Hz). Returns 0 or -errno. */
static inline int hwio_i2c_config(unsigned bus, unsigned hz) {
    return (int)_vm_sys2(SYS_I2C_CONFIG, bus, hz);
}

/* Combined transaction to 7-bit `addr`: write `wlen` bytes, then
 * (repeated START) read `rlen` bytes. Either half may be empty.
 * Returns 0 or -errno. */
static inline int hwio_i2c_xfer(unsigned bus, unsigned addr,
                                const void *wbuf, unsigned wlen,
                                void *rbuf, unsigned rlen) {
    return (int)_vm_sys6(SYS_I2C_XFER, bus, addr,
                         (uint32_t)(uintptr_t)wbuf, wlen,
                         (uint32_t)(uintptr_t)rbuf, rlen);
}

/* Convenience: point at register `reg`, then read `n` bytes — the
 * canonical sensor read (e.g. a temperature register). */
static inline int hwio_i2c_read_reg(unsigned bus, unsigned addr,
                                    uint8_t reg, void *buf, unsigned n) {
    return hwio_i2c_xfer(bus, addr, &reg, 1, buf, n);
}

/* Convenience: write one byte `val` to register `reg`. */
static inline int hwio_i2c_write_reg8(unsigned bus, unsigned addr,
                                      uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return hwio_i2c_xfer(bus, addr, b, 2, (void *)0, 0);
}

/* ---- SPI ---- */

/* Configure an SPI bus. `mode` is 0..3 (CPOL<<1 | CPHA). Returns 0
 * or -errno. */
static inline int hwio_spi_config(unsigned bus, unsigned mode, unsigned hz) {
    return (int)_vm_sys3(SYS_SPI_CONFIG, bus, mode, hz);
}

/* Full-duplex transfer of `len` bytes with chip-select `cs`. `tx`
 * or `rx` may be NULL for half-duplex. Returns 0 or -errno. */
static inline int hwio_spi_xfer(unsigned bus, unsigned cs,
                                const void *tx, void *rx, unsigned len) {
    return (int)_vm_sys5(SYS_SPI_XFER, bus, cs,
                         (uint32_t)(uintptr_t)tx,
                         (uint32_t)(uintptr_t)rx, len);
}

/* ---- ADC ---- */

/* Read an analog channel. Returns the raw value (>= 0) or -errno
 * (-ENOSYS on targets with no ADC, e.g. a bare Pi Zero). */
static inline int hwio_adc_read(unsigned channel) {
    return (int)_vm_sys1(SYS_ADC_READ, channel);
}

/* ---- PWM ---- */

/* Configure a PWM channel frequency (Hz). Returns 0 or -errno. */
static inline int hwio_pwm_config(unsigned channel, unsigned hz) {
    return (int)_vm_sys2(SYS_PWM_CONFIG, channel, hz);
}

/* Set a PWM channel duty cycle. `duty_q16` is a 0..65536 fraction
 * (65536 = 100%). Returns 0 or -errno. */
static inline int hwio_pwm_set(unsigned channel, uint32_t duty_q16) {
    return (int)_vm_sys2(SYS_PWM_SET, channel, duty_q16);
}

#endif /* GUEST_HWIO_H */
