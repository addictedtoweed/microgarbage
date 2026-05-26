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

int main(void) {
    TEST_SUITE("course");
    RUN(test_sample_passes_through_nodes);
    RUN(test_bake_carves_channel);
    RUN(test_bake_tunnel_and_open);
    return TEST_SUITE_RESULT();
}
