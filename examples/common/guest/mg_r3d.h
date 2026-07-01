/* ============================================================
 *  mg_r3d.h — cart-side guest API for the native 3D renderer.
 *
 *  The 3D renderer itself runs in the coprocessor firmware (host-side
 *  r3d): the guest does NOT transform or rasterize geometry. Instead the
 *  guest issues high-level scene commands — add objects, move/rotate
 *  them, set the camera — and asks the firmware to render + stage a
 *  frame to the SNES. (The rasterizer's z-buffer alone is ~230 KB, far
 *  past the guest's 64 KB data region, so rendering MUST be native.)
 *
 *  Coordinates and angles are Q16.16 fixed-point (1.0 == 1<<16). World
 *  units are arbitrary; the camera's focal length sets the on-screen
 *  scale. Object orientation is XYZ Euler angles in radians.
 *
 *  Typical loop:
 *      mg_r3d_reset();
 *      int cube = mg_r3d_add(MG_R3D_MESH_CUBE);
 *      for (;;) {
 *          mg_r3d_move  (cube, bx, by, MG_Q16(6));      // bounce
 *          mg_r3d_rotate(cube, ax, ay, 0);              // tumble
 *          mg_r3d_render();                             // firmware draws + stages
 *          mg_wait_frame();                             // pace to the SNES
 *      }
 *
 *  See docs/3d-renderer.md and the [[copro-3d-service]] design.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_R3D_H
#define MG_R3D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Q16.16 helper: whole number -> fixed-point. */
#define MG_Q16(n) ((int32_t)((n) * 65536))

/* Built-in mesh ids (more added as the firmware grows a mesh table). */
enum {
    MG_R3D_MESH_CUBE = 0,
};

/* Clear the scene to empty and arm a clean-slate VRAM wipe for the next
 * rendered frame. Installs a sane default camera (eye at origin looking
 * down +z, focal ~ screen width). Call once at startup. */
void mg_r3d_reset(void);

/* Set the camera: eye position (x,y,z) + orientation as yaw/pitch/roll
 * Euler radians + focal length in screen pixels. All Q16.16. Optional —
 * mg_r3d_reset installs a usable default. */
void mg_r3d_camera(int32_t eye_x, int32_t eye_y, int32_t eye_z,
                   int32_t yaw,   int32_t pitch, int32_t roll,
                   int32_t focal);

/* Add an instance of a built-in mesh to the scene. Returns an object
 * handle (>= 0) to address it later, or -1 if the scene is full. New
 * objects start at the origin, no rotation, visible. */
int  mg_r3d_add(uint8_t mesh_id);

/* MOVE / TRANSLATE: set object `obj`'s world position (Q16.16). */
void mg_r3d_move  (int obj, int32_t x, int32_t y, int32_t z);

/* ROTATE: set object `obj`'s orientation, XYZ Euler radians (Q16.16). */
void mg_r3d_rotate(int obj, int32_t rx, int32_t ry, int32_t rz);

/* Show or hide an object without removing it. */
void mg_r3d_show  (int obj, int visible);

/* Render the current scene in the firmware and stage one frame to the
 * SNES (the firmware does transform + raster + the 4bpp tile encode +
 * cart-window staging). Pair with mg_wait_frame() to pace. */
void mg_r3d_render(void);

#ifdef __cplusplus
}
#endif

#endif /* MG_R3D_H */
