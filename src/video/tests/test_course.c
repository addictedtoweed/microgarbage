/* test_course.c — headless tests for the course spline + baker.
 *
 *   cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
 *      -o build/tests/course src/video/tests/test_course.c src/video/course.c
 *
 * Public domain (CC0). No warranty.
 */
#include "test_runner.h"
#include "video/course.h"

#include <string.h>

#define SZ 256
static uint8_t Floor[SZ * SZ], Ceil[SZ * SZ], Mat[SZ * SZ];

static CourseNode mk(float x, float z, float y, float w, float wall, float ceil, float lava) {
    CourseNode n = { x, z, y, w, wall, ceil, lava };
    return n;
}
static unsigned cell(int x, int z) { return (unsigned)z * SZ + (unsigned)x; }

/* Catmull-Rom passes through its control points: sample at integer s
 * returns the node exactly (t=0 => p1). */
static void test_sample_passes_through_nodes(void) {
    CourseDef c;
    memset(&c, 0, sizeof c);
    c.count = 4; c.loop = false;
    c.node[0] = mk(40, 30, 50, 16, 100, 0, 4);
    c.node[1] = mk(80, 60, 60, 16, 100, 0, 4);
    c.node[2] = mk(120, 90, 45, 16, 100, 0, 4);
    c.node[3] = mk(160, 120, 70, 16, 100, 0, 4);

    CourseNode o;
    course_sample(&c, 0.0f, &o);
    ASSERT_EQ_INT(40, (int)o.x); ASSERT_EQ_INT(30, (int)o.z); ASSERT_EQ_INT(50, (int)o.y);
    course_sample(&c, 2.0f, &o);
    ASSERT_EQ_INT(120, (int)o.x); ASSERT_EQ_INT(90, (int)o.z); ASSERT_EQ_INT(45, (int)o.y);
    /* midway between node 1 and 2 stays between their x */
    course_sample(&c, 1.5f, &o);
    ASSERT(o.x > 80.0f && o.x < 120.0f);
}

static void test_bake_carves_channel(void) {
    CourseDef c;
    memset(&c, 0, sizeof c);
    c.count = 2; c.loop = false;
    /* a straight channel along z at x=60: floor y=40, width 12, walls 130,
     * open ceiling, lava river half-width 4. */
    c.node[0] = mk(60, 60, 40, 12, 130, 0, 4);
    c.node[1] = mk(60, 120, 40, 12, 130, 0, 4);
    course_bake(&c, Floor, Ceil, Mat, SZ);

    /* path centre: carved to the floor height, lava, open sky */
    ASSERT_EQ_INT(40, (int)Floor[cell(60, 90)]);
    ASSERT_EQ_INT(COURSE_MAT_LAVA, (int)Mat[cell(60, 90)]);
    ASSERT_EQ_INT((int)COURSE_OPEN_CEIL, (int)Ceil[cell(60, 90)]);

    /* corridor edge: floor risen toward the wall, rock (not lava) */
    ASSERT(Floor[cell(70, 90)] > 100);
    ASSERT_EQ_INT(COURSE_MAT_ROCK, (int)Mat[cell(70, 90)]);

    /* far outside the path: solid rock wall, untouched */
    ASSERT_EQ_INT((int)COURSE_WALL_H, (int)Floor[cell(200, 200)]);
    ASSERT_EQ_INT(COURSE_MAT_ROCK, (int)Mat[cell(200, 200)]);
}

static void test_bake_tunnel_and_open(void) {
    CourseDef c;
    memset(&c, 0, sizeof c);
    c.count = 2; c.loop = false;
    /* tunnel segment: ceil = 30 above the floor -> a low ceiling */
    c.node[0] = mk(100, 40, 50, 12, 120, 30, 0);
    c.node[1] = mk(100, 100, 50, 12, 120, 30, 0);
    course_bake(&c, Floor, Ceil, Mat, SZ);
    ASSERT_EQ_INT(80, (int)Ceil[cell(100, 70)]);     /* node.y + ceil = 50 + 30 */

    /* now open (ceil 0) -> sky again */
    c.node[0].ceil = 0; c.node[1].ceil = 0;
    course_bake(&c, Floor, Ceil, Mat, SZ);
    ASSERT_EQ_INT((int)COURSE_OPEN_CEIL, (int)Ceil[cell(100, 70)]);
}

/* Arc-length table is monotonic, totals the segment lengths, and maps
 * distance -> param so the endpoints and midpoint land where expected. */
static void test_arclen_maps_distance(void) {
    CourseDef c;
    memset(&c, 0, sizeof c);
    c.count = 2; c.loop = false;
    /* straight run along z from 60->120 at x=60: ~60 units, no y change */
    c.node[0] = mk(60, 60, 50, 16, 100, 0, 4);
    c.node[1] = mk(60, 120, 50, 16, 100, 0, 4);

    float total = course_total_distance(&c);
    ASSERT(total > 55.0f && total < 65.0f);          /* ~60 units */

    static CourseArc arc;                            /* large: keep off the stack */
    course_build_arc(&c, &arc);
    ASSERT(arc.total > 55.0f && arc.total < 65.0f);
    for (int i = 1; i <= COURSE_ARC_SAMPLES; i++)     /* cumulative is non-decreasing */
        ASSERT(arc.cum[i] >= arc.cum[i-1]);

    ASSERT(course_param_at_distance(&arc, 0.0f) == 0.0f);
    ASSERT(course_param_at_distance(&arc, arc.total) == arc.plen);
    /* halfway in distance => about halfway in z (straight line) */
    float s = course_param_at_distance(&arc, arc.total * 0.5f);
    CourseNode o; course_sample(&c, s, &o);
    ASSERT(o.z > 86.0f && o.z < 94.0f);
}

/* Generator: deterministic, in-bounds, descends, and total arc-length
 * grows with the requested duration. */
static void test_generate(void) {
    CourseDef a, b, a2;
    course_generate(1234u, 8.0f, 30.0f, 256.0f, &a);
    course_generate(1234u, 8.0f, 30.0f, 256.0f, &a2);
    course_generate(1234u, 2.0f, 30.0f, 256.0f, &b);

    ASSERT(a.count >= 4 && a.count <= COURSE_MAX_NODES);
    ASSERT(!a.loop);

    /* deterministic: same seed/args -> identical nodes */
    ASSERT_EQ_INT(a.count, a2.count);
    for (int i = 0; i < a.count; i++) {
        ASSERT((int)a.node[i].x == (int)a2.node[i].x);
        ASSERT((int)a.node[i].z == (int)a2.node[i].z);
    }

    /* in-bounds and descending high -> low */
    for (int i = 0; i < a.count; i++) {
        ASSERT(a.node[i].x >= 29.0f && a.node[i].x <= 227.0f);
        ASSERT(a.node[i].z >= 29.0f && a.node[i].z <= 227.0f);
    }
    ASSERT(a.node[0].y > a.node[a.count-1].y);

    /* longer duration -> longer course */
    ASSERT(a.count > b.count);
    ASSERT(course_total_distance(&a) > course_total_distance(&b));

    /* bakes without blowing up */
    course_bake(&a, Floor, Ceil, Mat, SZ);
}

int main(void) {
    TEST_SUITE("course");
    RUN(test_sample_passes_through_nodes);
    RUN(test_bake_carves_channel);
    RUN(test_bake_tunnel_and_open);
    RUN(test_arclen_maps_distance);
    RUN(test_generate);
    return TEST_SUITE_RESULT();
}
