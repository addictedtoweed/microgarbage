/* ============================================================
 *  host_hwio.h — host hardware-I/O backend interface
 *
 *  The platform layer that the SYS_GPIO_* / SYS_I2C_* / SYS_SPI_* /
 *  SYS_ADC_* / SYS_PWM_* ecalls call into. This is the seam between
 *  the (portable) ecall handlers in src/vm/vm_host_hwio.c and the
 *  (per-target) register-poking that actually drives the pins.
 *
 *  Same shape as host_platform.h: every function has a WEAK default
 *  in src/host/platform_hwio_stub.c that returns -VM_ENOSYS (or 0).
 *  A real target (platform_rpi.c → BCM2835 registers, platform_stm32.c
 *  → HAL) provides NON-weak overrides, one at a time, as it comes up.
 *  A desktop host may provide a simulated backend for testing, or
 *  install nothing (guests then get a clean -ENOSYS).
 *
 *  Buffers handed to these functions are ALREADY-TRANSLATED host
 *  pointers (the ecall handler does the guest→host bounds-checked
 *  translation), so a backend never touches VM internals — it deals
 *  only in plain host memory and hardware registers.
 *
 *  Return convention: >= 0 on success (0, or a value for readers),
 *  negative VM_E* (see vm_ecall.h) on failure. -VM_ENOSYS means "this
 *  target has no such peripheral", which is a valid, expected answer.
 *
 *  Concurrency: under the cooperative scheduler these run on the one
 *  host thread and need no locking. Under GARBAGE_SCHED_PREEMPTIVE,
 *  concurrent bus access from multiple VM threads would race — a
 *  per-bus lock is a follow-up (mirrors the fs_lock / audio_lock
 *  pattern), NOT yet implemented here. See docs/rpi-port.md.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_HOST_HWIO_H
#define MICROGARBAGE_HOST_HWIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO pin modes (the `mode` arg to host_hwio_gpio_config). Values
 * are ABI — keep in sync with HWIO_GPIO_* in the guest hwio.h. */
#define HWIO_GPIO_IN            0u   /* input, no pull            */
#define HWIO_GPIO_OUT           1u   /* output (push-pull)        */
#define HWIO_GPIO_IN_PULLUP     2u   /* input, pull-up enabled    */
#define HWIO_GPIO_IN_PULLDOWN   3u   /* input, pull-down enabled  */
#define HWIO_GPIO_OUT_OPENDRAIN 4u   /* output, open-drain        */

/* ---- GPIO ---- */

/* Configure a pin's direction/pull. Returns 0 or -errno. */
int host_hwio_gpio_config(uint32_t pin, uint32_t mode);

/* Drive an output pin. `level` is 0 or 1. Returns 0 or -errno. */
int host_hwio_gpio_write(uint32_t pin, uint32_t level);

/* Sample an input pin. Returns 0 or 1, or -errno. */
int host_hwio_gpio_read(uint32_t pin);

/* Flip an output pin. Returns 0 or -errno. */
int host_hwio_gpio_toggle(uint32_t pin);

/* Batched write: for every 1-bit in `mask`, set that pin of `bank`
 * to the corresponding bit of `values`, in one transaction. Returns
 * 0 or -errno. */
int host_hwio_gpio_write_mask(uint32_t bank, uint32_t mask, uint32_t values);

/* Batched read: sample the pins of `bank` selected by `mask` in one
 * transaction. Returns the sampled bits (reader idiom — no error
 * channel; an unconfigured/absent bank reads 0). */
uint32_t host_hwio_gpio_read_mask(uint32_t bank, uint32_t mask);

/* ---- I2C ---- */

/* Configure an I2C bus clock. Returns 0 or -errno. */
int host_hwio_i2c_config(uint32_t bus, uint32_t hz);

/* One combined transaction on `bus` to 7-bit `addr`: write `wlen`
 * bytes from `wbuf` (if wlen>0), then — with a repeated START — read
 * `rlen` bytes into `rbuf` (if rlen>0). Either half may be empty.
 * This is the canonical sensor access (write register pointer, read
 * N bytes). Returns 0 or -errno. */
int host_hwio_i2c_xfer(uint32_t bus, uint32_t addr,
                       const void *wbuf, uint32_t wlen,
                       void *rbuf, uint32_t rlen);

/* ---- SPI ---- */

/* Configure an SPI bus. `mode` is 0..3 (CPOL<<1 | CPHA). Returns 0
 * or -errno. */
int host_hwio_spi_config(uint32_t bus, uint32_t mode, uint32_t hz);

/* Full-duplex transfer of `len` bytes on `bus` with chip-select
 * `cs`: shift out `tx` while shifting in `rx`. Either pointer may be
 * NULL for a half-duplex (write-only / read-only) transfer. Returns
 * 0 or -errno. */
int host_hwio_spi_xfer(uint32_t bus, uint32_t cs,
                       const void *tx, void *rx, uint32_t len);

/* ---- ADC ---- */

/* Read an analog channel. Returns the raw value (>= 0) or -errno.
 * -VM_ENOSYS on targets with no ADC (e.g. a bare Pi Zero — wire an
 * external I2C/SPI ADC and use those instead). */
int host_hwio_adc_read(uint32_t channel);

/* ---- PWM ---- */

/* Configure a PWM channel frequency (Hz). Returns 0 or -errno. */
int host_hwio_pwm_config(uint32_t channel, uint32_t hz);

/* Set a PWM channel duty cycle. `duty_q16` is a 0..65536 fraction
 * (65536 = 100%); values above 65536 are clamped. Returns 0 or
 * -errno. */
int host_hwio_pwm_set(uint32_t channel, uint32_t duty_q16);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_HOST_HWIO_H */
