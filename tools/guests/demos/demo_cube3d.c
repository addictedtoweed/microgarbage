/* ============================================================
 *  demo_cube3d.c — drive the native 3D renderer: a tumbling, bouncing
 *  cube. The guest only issues scene commands (mg_r3d_*); the firmware
 *  transforms + rasterizes + encodes + stages each frame to the SNES.
 *  This is M1 of the coprocessor 3D port (see docs/3d-renderer.md).
 *
 *  START exits.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"
#include "mg_r3d.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void _start(void) {
    mg_r3d_reset();
    int cube = mg_r3d_add(MG_R3D_MESH_CUBE);

    const int32_t Z = MG_Q16(6);          /* depth in front of the camera */
    int32_t ax = 0, ay = 0;               /* tumble angles (Q16 radians)   */
    int32_t bx = 0, by = 0;               /* bounce position (Q16 world)   */
    int32_t vx = 1250, vy = 875;          /* bounce velocity / iter (slow — the
                                           * copro only samples at band 0, i.e.
                                           * every 4th call, so keep per-iter
                                           * motion small for tear-free bands) */
    const int32_t BX_LIM = MG_Q16(2);     /* +/- 2.0 world units            */
    const int32_t BY_LIM = (MG_Q16(3) / 2); /* +/- 1.5                      */

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) sys_exit(0);

        /* slow two-axis tumble (quartered — see vx/vy note) */
        ay += 725;
        ax += 475;

        /* bounce within the playfield */
        bx += vx; if (bx >  BX_LIM) { bx =  BX_LIM; vx = -vx; }
                  else if (bx < -BX_LIM) { bx = -BX_LIM; vx = -vx; }
        by += vy; if (by >  BY_LIM) { by =  BY_LIM; vy = -vy; }
                  else if (by < -BY_LIM) { by = -BY_LIM; vy = -vy; }

        mg_r3d_move(cube, bx, by, Z);
        mg_r3d_rotate(cube, ax, ay, 0);
        mg_r3d_render();                  /* firmware draws + stages the frame */
        mg_wait_frame();                  /* pace to the SNES */
    }
}
