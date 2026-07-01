/* ============================================================
 *  copro_r3d.h — native coprocessor-firmware 3D renderer service.
 *
 *  The 3D renderer runs HOST-SIDE (native firmware), not in the RISC-V
 *  guest: the guest issues scene commands (add/move/rotate objects, set
 *  camera) via SYS_R3D_* ecalls, and this service transforms + rasterizes
 *  (src/video/r3d.c) + encodes to 4bpp tiles + stages a frame through the
 *  existing mg_state cart-window transport. See docs/3d-renderer.md and
 *  the [[copro-3d-service]] design note.
 *
 *  M1: single 4bpp BG1 layer, ~16 colours, single-buffered in-place via
 *  mg_state_build_frame. M2 will add the 2bpp BG3 sub layer (the 60-colour
 *  LUT split) for the high-colour look.
 *
 *  Coordinates/angles are Q16.16. Implementation detail of mgapi — NOT in
 *  the public mgapi.h ABI.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_COPRO_R3D_H
#define MGAPI_COPRO_R3D_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in mesh ids (mirror examples/common/guest/mg_r3d.h). */
enum { COPRO_R3D_MESH_CUBE = 0 };

void copro_r3d_init    (void);
void copro_r3d_shutdown(void);

/* Clear the scene, install the default camera, arm a clean-slate VRAM
 * wipe + palette upload for the next rendered frame. */
void copro_r3d_reset(void);

/* Camera: eye position + yaw/pitch/roll Euler radians + focal (px). Q16.16. */
void copro_r3d_set_camera(int32_t ex, int32_t ey, int32_t ez,
                          int32_t yaw, int32_t pitch, int32_t roll,
                          int32_t focal);

/* Add an instance of a built-in mesh. Returns object handle (>=0) or -1. */
int  copro_r3d_add(uint8_t mesh_id);

/* Per-object transform / visibility (Q16.16 pos, Q16.16 Euler radians). */
void copro_r3d_object_move  (int obj, int32_t x,  int32_t y,  int32_t z);
void copro_r3d_object_rotate(int obj, int32_t rx, int32_t ry, int32_t rz);
void copro_r3d_object_show  (int obj, bool visible);

/* Render the current scene and stage one frame to the SNES via mg_state.
 * Returns 0 on stage, -1 if the previous frame is still in flight (the
 * caller should mg_wait_frame and retry). */
int  copro_r3d_render(void);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_COPRO_R3D_H */
