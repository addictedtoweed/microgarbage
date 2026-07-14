/* ============================================================
 *  platform_hwio_sim.c — DESKTOP SIMULATION hardware-I/O backend
 *
 *  A functional, host-side stand-in for host_hwio.h (include/vm/
 *  host_hwio.h) so the GPIO/I2C/SPI/ADC/PWM ecalls can be exercised
 *  end-to-end on a PC with no real hardware. Link this INSTEAD of
 *  platform_hwio_stub.c when you want the calls to actually do
 *  something (a fake GPIO bank + a fake I2C temperature sensor).
 *
 *  This is the reference the real platform_rpi.c / platform_stm32.c
 *  backends mirror: same signatures, real registers instead of these
 *  in-memory fakes.
 *
 *  Deterministic on purpose (no wall-clock / RNG) so demos and tests
 *  reproduce.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/host_hwio.h"
#include "vm/vm_ecall.h"   /* VM_E* error codes */

#include <stdint.h>

#define SIM_NPINS   64u

/* fake pin bank */
static struct { uint8_t mode; uint8_t level; } g_pins[SIM_NPINS];

/* one fake LM75-style I2C temperature sensor at 7-bit address 0x48.
 * 25.5 C encoded LM75-style: high byte = whole degrees, bit7 of the
 * low byte = the 0.5 C fraction. */
#define SIM_TEMP_ADDR  0x48u
#define SIM_TEMP_HI    0x19u   /* 25   */
#define SIM_TEMP_LO    0x80u   /* .5   */

/* ---- GPIO ---- */

int host_hwio_gpio_config(uint32_t pin, uint32_t mode) {
    if (pin >= SIM_NPINS) return -VM_EINVAL;
    g_pins[pin].mode = (uint8_t)mode;
    /* model the pull on inputs so a read after config is sensible */
    if (mode == HWIO_GPIO_IN_PULLUP)   g_pins[pin].level = 1;
    else if (mode == HWIO_GPIO_IN ||
             mode == HWIO_GPIO_IN_PULLDOWN) g_pins[pin].level = 0;
    return 0;
}

int host_hwio_gpio_write(uint32_t pin, uint32_t level) {
    if (pin >= SIM_NPINS) return -VM_EINVAL;
    g_pins[pin].level = level ? 1u : 0u;
    return 0;
}

int host_hwio_gpio_read(uint32_t pin) {
    if (pin >= SIM_NPINS) return -VM_EINVAL;
    return g_pins[pin].level;
}

int host_hwio_gpio_toggle(uint32_t pin) {
    if (pin >= SIM_NPINS) return -VM_EINVAL;
    g_pins[pin].level ^= 1u;
    return 0;
}

int host_hwio_gpio_write_mask(uint32_t bank, uint32_t mask, uint32_t values) {
    uint32_t base = bank * 32u;
    for (uint32_t b = 0; b < 32u; b++) {
        if (!(mask & (1u << b))) continue;
        uint32_t pin = base + b;
        if (pin < SIM_NPINS) g_pins[pin].level = (values >> b) & 1u;
    }
    return 0;
}

uint32_t host_hwio_gpio_read_mask(uint32_t bank, uint32_t mask) {
    uint32_t base = bank * 32u, out = 0;
    for (uint32_t b = 0; b < 32u; b++) {
        if (!(mask & (1u << b))) continue;
        uint32_t pin = base + b;
        if (pin < SIM_NPINS && g_pins[pin].level) out |= (1u << b);
    }
    return out;
}

/* ---- I2C ---- */

int host_hwio_i2c_config(uint32_t bus, uint32_t hz) {
    (void)bus; (void)hz;
    return 0;
}

int host_hwio_i2c_xfer(uint32_t bus, uint32_t addr,
                       const void *wbuf, uint32_t wlen,
                       void *rbuf, uint32_t rlen) {
    (void)bus;
    if (addr != SIM_TEMP_ADDR) return -VM_EIO;   /* NAK: nothing there */

    uint32_t reg = 0;
    if (wlen && wbuf) reg = ((const uint8_t *)wbuf)[0];

    if (rlen && rbuf) {
        uint8_t *r = (uint8_t *)rbuf;
        for (uint32_t i = 0; i < rlen; i++) r[i] = 0;
        if (reg == 0x00) {                       /* temperature register */
            if (rlen >= 1) r[0] = SIM_TEMP_HI;
            if (rlen >= 2) r[1] = SIM_TEMP_LO;
        }
    }
    return 0;
}

/* ---- SPI ---- */

int host_hwio_spi_config(uint32_t bus, uint32_t mode, uint32_t hz) {
    (void)bus; (void)mode; (void)hz;
    return 0;
}

int host_hwio_spi_xfer(uint32_t bus, uint32_t cs,
                       const void *tx, void *rx, uint32_t len) {
    (void)bus; (void)cs; (void)tx;
    if (rx && len) {                             /* idle MISO reads as 0xFF */
        uint8_t *r = (uint8_t *)rx;
        for (uint32_t i = 0; i < len; i++) r[i] = 0xFF;
    }
    return 0;
}

/* ---- ADC ---- */

int host_hwio_adc_read(uint32_t channel) {
    /* deterministic fake 12-bit reading, distinct per channel */
    return (int)(512u + (channel & 0x7u) * 100u);
}

/* ---- PWM ---- */

int host_hwio_pwm_config(uint32_t channel, uint32_t hz) {
    (void)channel; (void)hz;
    return 0;
}

int host_hwio_pwm_set(uint32_t channel, uint32_t duty_q16) {
    (void)channel; (void)duty_q16;
    return 0;
}
