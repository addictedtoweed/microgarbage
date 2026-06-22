/* ============================================================
 *  fmv_ecalls.c — SYS_FMV_PLAY / STOP / STATUS
 *
 *  Thin handlers over the host FMV player (fmv_player.h). The guest
 *  opens the .fmv with fs_open and hands the fd to PLAY; the player owns
 *  it from then on. No guest-pointer translation is needed — these pass
 *  scalars only.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "fmv_ecalls.h"

#include "fmv_player.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"

#include <errno.h>

/* SYS_FMV_PLAY(fd) → 0 or -errno; on success the player owns fd. */
static void h_fmv_play(VmCpu *cpu, void *system) {
    (void)system;
    int fd = (int)cpu->regs[VM_REG_A0];
    cpu->regs[VM_REG_A0] = fmv_player_start(fd) ? 0u : (uint32_t)-EINVAL;
}

/* SYS_FMV_STOP() → 0 */
static void h_fmv_stop(VmCpu *cpu, void *system) {
    (void)system;
    fmv_player_stop();
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_FMV_STATUS() → FMV_STATUS_* */
static void h_fmv_status(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = (uint32_t)fmv_player_status();
}

/* SYS_FMV_SET_HTIME(htime) → 0; live siphon force-blank tune */
static void h_fmv_set_htime(VmCpu *cpu, void *system) {
    (void)system;
    fmv_player_set_htime((uint8_t)cpu->regs[VM_REG_A0]);
    cpu->regs[VM_REG_A0] = 0;
}

bool mgapi_install_fmv_ecalls(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;
    VmEcallRouter *r = sys->ecall_router;

    if (!vm_ecall_register(r, SYS_FMV_PLAY,   h_fmv_play))   return false;
    if (!vm_ecall_register(r, SYS_FMV_STOP,   h_fmv_stop))   return false;
    if (!vm_ecall_register(r, SYS_FMV_STATUS, h_fmv_status)) return false;
    if (!vm_ecall_register(r, SYS_FMV_SET_HTIME, h_fmv_set_htime)) return false;
    return true;
}
