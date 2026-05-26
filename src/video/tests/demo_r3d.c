/* demo_r3d.c — MANUAL PoC: the fixed-point polygon renderer (r3d).
 *
 * Spins a flat-shaded cube to prove the transform -> project -> raster
 * pipeline + the camera/object affine3 transforms, rendered into the
 * same 8bpp framebuffer the Mode-7 path stretches to 256x224. This is
 * the RAM-light replacement for the heightfield raycaster: the world is
 * geometry (a handful of verts), not a multi-megabyte grid.
 *
 * Build + run (Windows):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_r3d \
 *      src/video/ppu.c src/video/present_gl_win32.c src/video/r3d.c \
 *      src/math/trig_q16.c src/math/fixed_point.c \
 *      src/video/tests/demo_r3d.c -lopengl32 -lgdi32 -luser32
 * Headless ASCII check: add -DR3D_HEADLESS, link without present/GL.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/r3d.h"
#ifndef R3D_HEADLESS
#include "video/present.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>

#define FBW 128
#define FBH 112
#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))
#define CUBE_BASE 1
#define CUBE_RAMP 24

static PpuState P;
static uint8_t  fbuf[FBW * FBH];
#ifndef R3D_HEADLESS
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
#endif

/* unit cube */
static const vec3_q16 cube_v[8] = {
    { -Q16_ONE, -Q16_ONE, -Q16_ONE }, {  Q16_ONE, -Q16_ONE, -Q16_ONE },
    {  Q16_ONE,  Q16_ONE, -Q16_ONE }, { -Q16_ONE,  Q16_ONE, -Q16_ONE },
    { -Q16_ONE, -Q16_ONE,  Q16_ONE }, {  Q16_ONE, -Q16_ONE,  Q16_ONE },
    {  Q16_ONE,  Q16_ONE,  Q16_ONE }, { -Q16_ONE,  Q16_ONE,  Q16_ONE },
};
static const uint16_t cube_t[36] = {       /* consistent CCW-outward winding */
    4,5,6, 4,6,7,   /* +z front  */
    0,3,2, 0,2,1,   /* -z back   */
    1,2,6, 1,6,5,   /* +x right  */
    0,4,7, 0,7,3,   /* -x left   */
    3,7,6, 3,6,2,   /* +y top    */
    0,1,5, 0,5,4,   /* -y bottom */
};
static const R3dMesh cube = { cube_v, 8, cube_t, 12, NULL };

static uint16_t lerp555(uint16_t a, uint16_t b, int num, int den) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (br - ar) * num / den, ag + (bg - ag) * num / den, ab + (bb - ab) * num / den);
}
static void build_palette(void) {
    ppu_state_clear(&P);
    P.mode = 7;
    P.brightness = 15;
    P.cgram[0] = BGR555(2, 2, 5);                                  /* dark backdrop */
    for (int s = 0; s < CUBE_RAMP; s++)                            /* navy -> light cyan */
        P.cgram[CUBE_BASE + s] = lerp555(BGR555(3, 5, 9), BGR555(20, 30, 31), s, CUBE_RAMP - 1);
}

#ifndef R3D_HEADLESS
/* pack the 8bpp fbuf into the Mode-7 plane and set the scale-only matrix */
static void load_mode7(void) {
    const int TW = FBW / 8;
    for (int ty = 0; ty < FBH / 8; ty++)
        for (int tx = 0; tx < TW; tx++) {
            unsigned tile = (unsigned)(ty * TW + tx);
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    uint8_t idx = fbuf[(ty * 8 + py) * FBW + (tx * 8 + px)];
                    unsigned w = (tile * 64u + (unsigned)py * 8u + (unsigned)px) & 0x7FFFu;
                    P.vram[w] = (uint16_t)((P.vram[w] & 0x00FFu) | ((unsigned)idx << 8));
                }
            unsigned mw = ((unsigned)ty * 128u + (unsigned)tx) & 0x7FFFu;
            P.vram[mw] = (uint16_t)((P.vram[mw] & 0xFF00u) | (tile & 0xFFu));
        }
    P.m7a = (int16_t)FBW;
    P.m7d = (int16_t)(256 * FBH / PPU_SCREEN_H);
    P.m7b = 0; P.m7c = 0; P.m7x = 0; P.m7y = 0; P.m7hofs = 0; P.m7vofs = 0;
}
#endif

static R3dObject obj;
static R3dScene  scene;

static void build_scene(void) {
    obj.mesh = &cube;
    obj.xform = affine3_q16_identity();

    /* camera 5 units back on -z, looking +z */
    scene.view = affine3_q16_view(vec3_q16_from_int(0, 0, -5),
                                  vec3_q16_from_int(1, 0, 0),
                                  vec3_q16_from_int(0, 1, 0),
                                  vec3_q16_from_int(0, 0, 1));
    scene.focal   = q16_from_int(110);
    scene.near_z  = q16_from_double(0.25);
    scene.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(-0.3),
                                                     q16_from_double(0.5),
                                                     q16_from_double(-0.8)));
    scene.ambient = q16_from_double(0.30);
    scene.diffuse = q16_from_double(0.70);
    scene.base    = CUBE_BASE;
    scene.ramp    = CUBE_RAMP;
    scene.objs    = &obj;
    scene.nobjs   = 1;
}

static void spin(q16_16_t ry, q16_16_t rx) {
    obj.xform = affine3_q16_from_rotation(
        mat3_q16_mul(mat3_q16_rotation_y(ry), mat3_q16_rotation_x(rx)));
}

#ifdef R3D_HEADLESS
int main(void) {
    build_palette();
    build_scene();
    spin(q16_from_double(0.6), q16_from_double(0.4));
    r3d_render(&scene, fbuf, FBW, FBH);
    for (int y = 0; y < FBH; y += 3) {
        for (int x = 0; x < FBW; x += 2) {
            uint8_t v = fbuf[y * FBW + x];
            char ch;
            if (v == 0) ch = ' ';
            else { int t = v - CUBE_BASE; ch = t < 8 ? '.' : t < 16 ? ':' : '#'; }
            putchar(ch);
        }
        putchar('\n');
    }
    return 0;
}
#else
static double now_sec(void) {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - r3d polygon test")) return 1;
    printf("GL: %s\nr3d: spinning flat-shaded cube (%dx%d -> Mode 7)\n", present_gl_renderer(), FBW, FBH);
    fflush(stdout);

    build_palette();
    build_scene();
    double t0 = now_sec(), report = t0;
    unsigned frames = 0;
    while (!present_should_close()) {
        float t = (float)(now_sec() - t0);
        spin(q16_from_double(t * 0.7), q16_from_double(t * 0.5));
        r3d_render(&scene, fbuf, FBW, FBH);
        load_mode7();
        ppu_render(&P, FB);
        present_frame(FB);
        frames++;
        double now = now_sec();
        if (now - report >= 1.0) { printf("  %.1f fps\n", (double)frames / (now - report));
            fflush(stdout); report = now; frames = 0; }
    }
    present_shutdown();
    return 0;
}
#endif
