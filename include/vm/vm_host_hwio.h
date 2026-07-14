/* ============================================================
 *  vm_host_hwio.h — install the hardware-I/O ecalls
 *
 *  Registers the SYS_GPIO_* / SYS_I2C_* / SYS_SPI_* / SYS_ADC_* /
 *  SYS_PWM_* handlers on a VmSystem's ecall router. The handlers do
 *  the guest→host pointer translation and forward to the host_hwio_*
 *  backend (include/vm/host_hwio.h), which the target implements.
 *
 *  Opt-in, like vm_host_install_fs / vm_host_install_stdio: a host
 *  with no hardware (a desktop build) simply doesn't call this, and
 *  the numbers stay unhandled. A host that DOES call it but has only
 *  the weak stub backend linked gives guests a clean -VM_ENOSYS.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_HOST_HWIO_H
#define MICROGARBAGE_VM_HOST_HWIO_H

#include <stdbool.h>
#include "vm/vm_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register all hardware-I/O handlers on sys->ecall_router. Returns
 * true on success; on failure it rolls back any it already
 * registered and returns false (so a partial install never leaks). */
bool vm_host_install_hwio(VmSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_VM_HOST_HWIO_H */
