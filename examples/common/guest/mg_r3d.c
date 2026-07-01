/* ============================================================
 *  mg_r3d.c — guest-side 3D renderer ecall stubs.
 *  See mg_r3d.h for the contract. The renderer runs in firmware;
 *  these just marshal scene commands across SYS_R3D_*.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_r3d.h"
#include "vm_runtime.h"

void mg_r3d_reset(void) {
    (void)_vm_sys0(SYS_R3D_RESET);
}

void mg_r3d_camera(int32_t eye_x, int32_t eye_y, int32_t eye_z,
                   int32_t yaw, int32_t pitch, int32_t roll, int32_t focal) {
    (void)_vm_sys7(SYS_R3D_CAMERA,
                   (uint32_t)eye_x, (uint32_t)eye_y, (uint32_t)eye_z,
                   (uint32_t)yaw, (uint32_t)pitch, (uint32_t)roll,
                   (uint32_t)focal);
}

int mg_r3d_add(uint8_t mesh_id) {
    return (int)(int32_t)_vm_sys1(SYS_R3D_ADD, (uint32_t)mesh_id);
}

void mg_r3d_move(int obj, int32_t x, int32_t y, int32_t z) {
    (void)_vm_sys4(SYS_R3D_MOVE, (uint32_t)obj,
                   (uint32_t)x, (uint32_t)y, (uint32_t)z);
}

void mg_r3d_rotate(int obj, int32_t rx, int32_t ry, int32_t rz) {
    (void)_vm_sys4(SYS_R3D_ROTATE, (uint32_t)obj,
                   (uint32_t)rx, (uint32_t)ry, (uint32_t)rz);
}

void mg_r3d_show(int obj, int visible) {
    (void)_vm_sys2(SYS_R3D_SHOW, (uint32_t)obj, visible ? 1u : 0u);
}

void mg_r3d_render(void) {
    (void)_vm_sys0(SYS_R3D_RENDER);
}
