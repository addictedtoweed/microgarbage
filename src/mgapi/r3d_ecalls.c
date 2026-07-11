/* ============================================================
 *  r3d_ecalls.c — SYS_R3D_* handlers over the native 3D renderer.
 *
 *  Thin scalar handlers (no guest-pointer translation): the guest sends
 *  Q16.16 transforms + scene commands and asks for a frame; the firmware
 *  (copro_r3d.c) does transform + raster + the 4bpp encode + cart-window
 *  staging. See examples/common/guest/mg_r3d.h.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "r3d_ecalls.h"

#include "copro_r3d.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"

#include <stdio.h>

/* SYS_R3D_RESET() → 0 */
static void h_r3d_reset(VmCpu *cpu, void *system) {
    (void)system;
    static int once = 0;
    if (!once) { once = 1;
        fprintf(stderr, "mgapi: SYS_R3D_RESET received -- cube3d guest is running\n"); }
    copro_r3d_reset();
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_R3D_CAMERA(ex,ey,ez, yaw,pitch,roll, focal) → 0 */
static void h_r3d_camera(VmCpu *cpu, void *system) {
    (void)system;
    copro_r3d_set_camera((int32_t)cpu->regs[VM_REG_A0],
                         (int32_t)cpu->regs[VM_REG_A1],
                         (int32_t)cpu->regs[VM_REG_A2],
                         (int32_t)cpu->regs[VM_REG_A3],
                         (int32_t)cpu->regs[VM_REG_A4],
                         (int32_t)cpu->regs[VM_REG_A5],
                         (int32_t)cpu->regs[VM_REG_A6]);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_R3D_ADD(mesh_id) → object handle (>=0) or -1 */
static void h_r3d_add(VmCpu *cpu, void *system) {
    (void)system;
    int h = copro_r3d_add((uint8_t)cpu->regs[VM_REG_A0]);
    cpu->regs[VM_REG_A0] = (uint32_t)h;
}

/* SYS_R3D_MOVE(obj, x,y,z) → 0 */
static void h_r3d_move(VmCpu *cpu, void *system) {
    (void)system;
    copro_r3d_object_move((int)cpu->regs[VM_REG_A0],
                          (int32_t)cpu->regs[VM_REG_A1],
                          (int32_t)cpu->regs[VM_REG_A2],
                          (int32_t)cpu->regs[VM_REG_A3]);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_R3D_ROTATE(obj, rx,ry,rz) → 0 */
static void h_r3d_rotate(VmCpu *cpu, void *system) {
    (void)system;
    copro_r3d_object_rotate((int)cpu->regs[VM_REG_A0],
                            (int32_t)cpu->regs[VM_REG_A1],
                            (int32_t)cpu->regs[VM_REG_A2],
                            (int32_t)cpu->regs[VM_REG_A3]);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_R3D_SHOW(obj, visible) → 0 */
static void h_r3d_show(VmCpu *cpu, void *system) {
    (void)system;
    copro_r3d_object_show((int)cpu->regs[VM_REG_A0],
                          cpu->regs[VM_REG_A1] != 0);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_R3D_RENDER() → 0 staged / -1 in-flight */
static void h_r3d_render(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = (uint32_t)copro_r3d_render();
}

bool mgapi_install_r3d_ecalls(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;
    VmEcallRouter *r = sys->ecall_router;

    copro_r3d_init();

    if (!vm_ecall_register(r, SYS_R3D_RESET,  h_r3d_reset))  return false;
    if (!vm_ecall_register(r, SYS_R3D_CAMERA, h_r3d_camera)) return false;
    if (!vm_ecall_register(r, SYS_R3D_ADD,    h_r3d_add))    return false;
    if (!vm_ecall_register(r, SYS_R3D_MOVE,   h_r3d_move))   return false;
    if (!vm_ecall_register(r, SYS_R3D_ROTATE, h_r3d_rotate)) return false;
    if (!vm_ecall_register(r, SYS_R3D_SHOW,   h_r3d_show))   return false;
    if (!vm_ecall_register(r, SYS_R3D_RENDER, h_r3d_render)) return false;
    return true;
}
