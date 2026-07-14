/* ============================================================
 *  vm_host_hwio.c — hardware-I/O ecall handlers
 *
 *  Backs SYS_GPIO_* / SYS_I2C_* / SYS_SPI_* / SYS_ADC_* / SYS_PWM_*
 *  (1250..1279, see vm_ecall.h). Each handler reads its args from
 *  cpu->regs[A0..A5], translates any guest buffer pointers to
 *  bounds-checked host pointers, forwards to the host_hwio_* backend
 *  (host_hwio.h), and writes the result to A0.
 *
 *  These are TRANSACTIONS: a whole I2C/SPI transfer happens in one
 *  ecall, natively in the backend — the interpreter is never in the
 *  per-bit path. See docs/rpi-port.md.
 *
 *  Concurrency: safe under the cooperative scheduler (single host
 *  thread). Under GARBAGE_SCHED_PREEMPTIVE a per-bus lock is a
 *  follow-up (mirror fs_lock / audio_lock); not yet added.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stddef.h>

#include "vm/vm_host_hwio.h"
#include "vm/vm_core.h"     /* VmCpu, VM_REG_A*, vm_translate_*, VmEcallHandler */
#include "vm/vm_ecall.h"    /* SYS_*, VM_E*, vm_ecall_register/unregister */
#include "vm/host_hwio.h"   /* the backend */

/* Store a signed result into A0 the way the FS handlers do: a
 * negative errno becomes its two's-complement uint32. */
static inline void ret_i(VmCpu *cpu, int v) {
    cpu->regs[VM_REG_A0] = (uint32_t)v;
}

/* ---- GPIO ---- */

static void handle_gpio_config(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_gpio_config(cpu->regs[VM_REG_A0],
                                     cpu->regs[VM_REG_A1]));
}

static void handle_gpio_write(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_gpio_write(cpu->regs[VM_REG_A0],
                                    cpu->regs[VM_REG_A1]));
}

static void handle_gpio_read(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_gpio_read(cpu->regs[VM_REG_A0]));
}

static void handle_gpio_toggle(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_gpio_toggle(cpu->regs[VM_REG_A0]));
}

static void handle_gpio_write_mask(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_gpio_write_mask(cpu->regs[VM_REG_A0],
                                         cpu->regs[VM_REG_A1],
                                         cpu->regs[VM_REG_A2]));
}

static void handle_gpio_read_mask(VmCpu *cpu, void *system) {
    (void)system;
    /* reader idiom: raw sampled bits into A0, no error channel */
    cpu->regs[VM_REG_A0] = host_hwio_gpio_read_mask(cpu->regs[VM_REG_A0],
                                                    cpu->regs[VM_REG_A1]);
}

/* ---- I2C ---- */

static void handle_i2c_config(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_i2c_config(cpu->regs[VM_REG_A0],
                                    cpu->regs[VM_REG_A1]));
}

static void handle_i2c_xfer(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t bus   = cpu->regs[VM_REG_A0];
    uint32_t addr  = cpu->regs[VM_REG_A1];
    uint32_t waddr = cpu->regs[VM_REG_A2];
    uint32_t wlen  = cpu->regs[VM_REG_A3];
    uint32_t raddr = cpu->regs[VM_REG_A4];
    uint32_t rlen  = cpu->regs[VM_REG_A5];

    const void *wbuf = NULL;
    void       *rbuf = NULL;
    if (wlen) {
        wbuf = vm_translate_read(cpu, waddr, wlen);
        if (!wbuf) { ret_i(cpu, -VM_EFAULT); return; }
    }
    if (rlen) {
        rbuf = vm_translate_write(cpu, raddr, rlen);
        if (!rbuf) { ret_i(cpu, -VM_EFAULT); return; }
    }
    ret_i(cpu, host_hwio_i2c_xfer(bus, addr, wbuf, wlen, rbuf, rlen));
}

/* ---- SPI ---- */

static void handle_spi_config(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_spi_config(cpu->regs[VM_REG_A0],
                                    cpu->regs[VM_REG_A1],
                                    cpu->regs[VM_REG_A2]));
}

static void handle_spi_xfer(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t bus     = cpu->regs[VM_REG_A0];
    uint32_t cs      = cpu->regs[VM_REG_A1];
    uint32_t txaddr  = cpu->regs[VM_REG_A2];
    uint32_t rxaddr  = cpu->regs[VM_REG_A3];
    uint32_t len     = cpu->regs[VM_REG_A4];

    const void *tx = NULL;
    void       *rx = NULL;
    if (len) {
        if (txaddr) {
            tx = vm_translate_read(cpu, txaddr, len);
            if (!tx) { ret_i(cpu, -VM_EFAULT); return; }
        }
        if (rxaddr) {
            rx = vm_translate_write(cpu, rxaddr, len);
            if (!rx) { ret_i(cpu, -VM_EFAULT); return; }
        }
    }
    ret_i(cpu, host_hwio_spi_xfer(bus, cs, tx, rx, len));
}

/* ---- ADC ---- */

static void handle_adc_read(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_adc_read(cpu->regs[VM_REG_A0]));
}

/* ---- PWM ---- */

static void handle_pwm_config(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_pwm_config(cpu->regs[VM_REG_A0],
                                    cpu->regs[VM_REG_A1]));
}

static void handle_pwm_set(VmCpu *cpu, void *system) {
    (void)system;
    ret_i(cpu, host_hwio_pwm_set(cpu->regs[VM_REG_A0],
                                 cpu->regs[VM_REG_A1]));
}

/* ---- install ---- */

static const struct { uint32_t num; VmEcallHandler fn; } k_hwio[] = {
    { SYS_GPIO_CONFIG,     handle_gpio_config     },
    { SYS_GPIO_WRITE,      handle_gpio_write      },
    { SYS_GPIO_READ,       handle_gpio_read       },
    { SYS_GPIO_TOGGLE,     handle_gpio_toggle     },
    { SYS_GPIO_WRITE_MASK, handle_gpio_write_mask },
    { SYS_GPIO_READ_MASK,  handle_gpio_read_mask  },
    { SYS_I2C_CONFIG,      handle_i2c_config      },
    { SYS_I2C_XFER,        handle_i2c_xfer        },
    { SYS_SPI_CONFIG,      handle_spi_config      },
    { SYS_SPI_XFER,        handle_spi_xfer        },
    { SYS_ADC_READ,        handle_adc_read        },
    { SYS_PWM_CONFIG,      handle_pwm_config      },
    { SYS_PWM_SET,         handle_pwm_set         },
};

bool vm_host_install_hwio(VmSystem *sys) {
    if (!sys) return false;
    const size_t n = sizeof(k_hwio) / sizeof(k_hwio[0]);
    for (size_t i = 0; i < n; i++) {
        if (!vm_ecall_register(sys->ecall_router, k_hwio[i].num, k_hwio[i].fn)) {
            for (size_t j = 0; j < i; j++) {
                vm_ecall_unregister(sys->ecall_router, k_hwio[j].num);
            }
            return false;
        }
    }
    return true;
}
