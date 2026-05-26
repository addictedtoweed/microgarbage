/* test_hfcast.c — headless tests for the fixed-point heightfield
 * raycaster. Renders known scenes and asserts framebuffer geometry +
 * shading, so the renderer is verified without a display.
 *
 *   cc ... -o build/tests/hfcast src/video/tests/test_hfcast.c src/video/hfcast.c
 *
 * Public domain (CC0). No warranty.
 */
#include "test_runner.h"
#include "video/hfcast.h"

#include <string.h>

#define SZ  64
#define FBW 32
#define FBH 28

static uint8_t Floor[SZ * SZ], Ceil[SZ * SZ], Mat[SZ * SZ];
static uint8_t FB[FBW * FBH];

enum { ROCK_BASE = 1, SNOW_BASE = 21, LAVA_BASE = 41, RAMP = 20 };

static HfScene make_scene(void) {
    HfScene s;
    s.floor = Floor; s.ceiling = Ceil; s.material = Mat; s.mapsz = SZ;
    s.light = vec3_q16_normalize(vec3_q16_make(q16_from_double(0.53),
                                               q16_from_double(0.74),
                                               q16_from_double(0.42)));
    s.fog_range = q16_from_int(130);
    s.rock_base = ROCK_BASE; s.snow_base = SNOW_BASE; s.lava_base = LAVA_BASE;
    s.ramp = RAMP;
    s.rock_mat = 0; s.snow_mat = 1; s.lava_mat = 2;
    s.sky_h = q16_from_int(220);     /* the wall-ahead test uses a 180-tall wall */
    return s;
}

/* camera at (32,60,8) looking +z, moderate FOV */
static HfCamera make_cam(void) {
    HfCamera c;
    c.pos   = vec3_q16_from_int(32, 60, 8);
    c.right = vec3_q16_from_int(1, 0, 0);
    c.up    = vec3_q16_from_int(0, 1, 0);
    c.fwd   = vec3_q16_from_int(0, 0, 1);
    c.halfw = q16_from_double(0.6);
    c.halfh = q16_from_double(0.6);
    return c;
}

static void fill(uint8_t *m, uint8_t v) { memset(m, v, SZ * SZ); }
static uint8_t px(int x, int y) { return FB[y * FBW + x]; }

static void test_flat_floor_and_sky(void) {
    fill(Floor, 50); fill(Ceil, 255); fill(Mat, 0);    /* flat rock, open sky */
    HfScene s = make_scene(); HfCamera c = make_cam();
    long steps = hfcast_render(&s, &c, FB, FBW, FBH);

    ASSERT(steps > 0);
    /* top-centre ray angles up -> escapes to sky (index 0) */
    ASSERT_EQ_INT(0, (int)px(FBW / 2, 0));
    /* bottom-centre ray angles down -> hits the rock floor */
    uint8_t b = px(FBW / 2, FBH - 1);
    ASSERT(b >= ROCK_BASE && b < ROCK_BASE + RAMP);
}

static void test_material_lava(void) {
    fill(Floor, 50); fill(Ceil, 255); fill(Mat, 2);    /* lava floor */
    HfScene s = make_scene(); HfCamera c = make_cam();
    hfcast_render(&s, &c, FB, FBW, FBH);

    uint8_t b = px(FBW / 2, FBH - 1);
    ASSERT(b >= LAVA_BASE && b < LAVA_BASE + RAMP);     /* lava ramp */
    /* lava is emissive (bright) -> high in its ramp */
    ASSERT(b >= LAVA_BASE + RAMP / 2);
}

static void test_wall_ahead(void) {
    HfScene s = make_scene(); HfCamera c = make_cam();

    /* no wall: the near-horizontal centre ray finds only floor far below
     * it -> never reaches it within range -> sky. */
    fill(Floor, 50); fill(Ceil, 255); fill(Mat, 0);
    hfcast_render(&s, &c, FB, FBW, FBH);
    ASSERT_EQ_INT(0, (int)px(FBW / 2, FBH / 2));

    /* now raise a tall wall band straight ahead -> the centre ray hits it */
    for (int z = 40; z < 48; z++)
        for (int x = 0; x < SZ; x++) Floor[z * SZ + x] = 180;
    hfcast_render(&s, &c, FB, FBW, FBH);
    ASSERT(px(FBW / 2, FBH / 2) != 0);
}

int main(void) {
    TEST_SUITE("hfcast");
    RUN(test_flat_floor_and_sky);
    RUN(test_material_lava);
    RUN(test_wall_ahead);
    return TEST_SUITE_RESULT();
}
