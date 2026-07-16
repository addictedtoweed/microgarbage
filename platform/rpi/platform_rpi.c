/* platform/rpi/platform_rpi.c -- BCM2835 hardware-IO backend.
 *
 * The real Pi backend behind the host_hwio.h seam (the desktop
 * src/host/platform_hwio_sim.c is its blueprint): the guest issues
 * portable SYS_GPIO / I2C / ... ecalls; the handlers in vm_host_hwio.c
 * translate + forward here, and here we poke actual BCM2835 registers.
 * The same guest .elf runs on the desktop sim and on this.
 *
 * GPIO is implemented and verifiable in QEMU (it models the GPIO
 * peripheral). I2C/SPI/ADC/PWM need attached slave devices to develop
 * and verify, which QEMU raspi0 does not model -- they return -ENOSYS
 * with a TODO until real-hardware bring-up (that IS the dev-kit
 * workflow this enables: plug in a sensor, write the driver against it).
 *
 * Public domain (CC0). No warranty.
 */

#include <stdint.h>

#include "vm/host_hwio.h"   /* HWIO_GPIO_* + the backend signatures */
#include "vm/vm_ecall.h"    /* VM_ENOSYS / VM_EINVAL */

#define PERIPH_BASE  0x20000000u
#define GPIO_BASE    (PERIPH_BASE + 0x200000u)

#define GPFSEL(n)    (*(volatile uint32_t *)(GPIO_BASE + 0x00 + (n) * 4u))
#define GPSET(n)     (*(volatile uint32_t *)(GPIO_BASE + 0x1C + (n) * 4u))
#define GPCLR(n)     (*(volatile uint32_t *)(GPIO_BASE + 0x28 + (n) * 4u))
#define GPLEV(n)     (*(volatile uint32_t *)(GPIO_BASE + 0x34 + (n) * 4u))
#define GPPUD        (*(volatile uint32_t *)(GPIO_BASE + 0x94))
#define GPPUDCLK(n)  (*(volatile uint32_t *)(GPIO_BASE + 0x98 + (n) * 4u))

#define NPINS  54u   /* BCM2835 GPIO0..53 */

static void short_delay(int n) { while (n-- > 0) __asm__ volatile("nop"); }

/* pud: 0 = none, 1 = pull-down, 2 = pull-up (the GPPUD/GPPUDCLK dance). */
static void set_pull(uint32_t pin, uint32_t pud) {
    GPPUD = pud;                          short_delay(150);
    GPPUDCLK(pin / 32u) = 1u << (pin % 32u); short_delay(150);
    GPPUDCLK(pin / 32u) = 0;
    GPPUD = 0;
}

/* ---- GPIO ---- */

int host_hwio_gpio_config(uint32_t pin, uint32_t mode) {
    if (pin >= NPINS) return -VM_EINVAL;
    uint32_t func, pud = 0;
    switch (mode) {
        case HWIO_GPIO_IN:            func = 0; pud = 0; break;
        case HWIO_GPIO_IN_PULLUP:     func = 0; pud = 2; break;
        case HWIO_GPIO_IN_PULLDOWN:   func = 0; pud = 1; break;
        case HWIO_GPIO_OUT:           func = 1; break;
        case HWIO_GPIO_OUT_OPENDRAIN: func = 1; break;  /* BCM2835 has no true OD */
        default: return -VM_EINVAL;
    }
    uint32_t reg = pin / 10u, shift = (pin % 10u) * 3u;
    uint32_t v = GPFSEL(reg);
    v &= ~(7u << shift);
    v |=  (func << shift);
    GPFSEL(reg) = v;
    set_pull(pin, pud);
    return 0;
}

int host_hwio_gpio_write(uint32_t pin, uint32_t level) {
    if (pin >= NPINS) return -VM_EINVAL;
    if (level) GPSET(pin / 32u) = 1u << (pin % 32u);
    else       GPCLR(pin / 32u) = 1u << (pin % 32u);
    return 0;
}

int host_hwio_gpio_read(uint32_t pin) {
    if (pin >= NPINS) return -VM_EINVAL;
    return (int)((GPLEV(pin / 32u) >> (pin % 32u)) & 1u);
}

int host_hwio_gpio_toggle(uint32_t pin) {
    if (pin >= NPINS) return -VM_EINVAL;
    uint32_t cur = (GPLEV(pin / 32u) >> (pin % 32u)) & 1u;
    return host_hwio_gpio_write(pin, cur ^ 1u);
}

int host_hwio_gpio_write_mask(uint32_t bank, uint32_t mask, uint32_t values) {
    if (bank > 1u) return -VM_EINVAL;
    GPSET(bank) = mask & values;         /* set the 1s ... */
    GPCLR(bank) = mask & ~values;        /* ... clear the 0s, among masked pins */
    return 0;
}

uint32_t host_hwio_gpio_read_mask(uint32_t bank, uint32_t mask) {
    if (bank > 1u) return 0;
    return GPLEV(bank) & mask;
}

/* ---- I2C / SPI / ADC / PWM ----
 * TODO(hw): real BCM2835 BSC (I2C @0x20804000), SPI0 (@0x20204000), and
 * PWM (@0x2020C000) drivers. QEMU raspi0 models neither these buses nor
 * any attached slave, so there is nothing to verify against here -- these
 * get written and tested on real hardware with a real device wired up
 * (the temp-sensor dev-kit workflow). BCM2835 has no ADC at all. */
int host_hwio_i2c_config(uint32_t bus, uint32_t hz) {
    (void)bus; (void)hz; return -VM_ENOSYS;
}
int host_hwio_i2c_xfer(uint32_t bus, uint32_t addr,
                       const void *wbuf, uint32_t wlen,
                       void *rbuf, uint32_t rlen) {
    (void)bus; (void)addr; (void)wbuf; (void)wlen; (void)rbuf; (void)rlen;
    return -VM_ENOSYS;
}
int host_hwio_spi_config(uint32_t bus, uint32_t mode, uint32_t hz) {
    (void)bus; (void)mode; (void)hz; return -VM_ENOSYS;
}
int host_hwio_spi_xfer(uint32_t bus, uint32_t cs,
                       const void *tx, void *rx, uint32_t len) {
    (void)bus; (void)cs; (void)tx; (void)rx; (void)len; return -VM_ENOSYS;
}
int host_hwio_adc_read(uint32_t channel) {
    (void)channel; return -VM_ENOSYS;   /* no ADC on BCM2835 */
}
int host_hwio_pwm_config(uint32_t channel, uint32_t hz) {
    (void)channel; (void)hz; return -VM_ENOSYS;
}
int host_hwio_pwm_set(uint32_t channel, uint32_t duty_q16) {
    (void)channel; (void)duty_q16; return -VM_ENOSYS;
}
