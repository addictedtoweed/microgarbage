/* ============================================================
 *  course.h — data-driven flight course: spline + map baker.
 *
 *  A course is a list of control NODES (a path Kestrel races, each
 *  carrying the look of the canyon at that point: corridor width, wall
 *  height, ceiling/tunnel, lava-river width). A Catmull-Rom spline runs
 *  smoothly through the nodes. course_bake() turns the course into the
 *  floor / ceiling / material heightfields the renderer consumes.
 *
 *  This module is PORTABLE (no stdio, no float-libm, no platform deps):
 *  the host tool bakes a course to preview it, and the cart bakes the
 *  same course at level-load. The text PARSER lives in the host tool;
 *  this header is just the data + spline + baker.
 *
 *  Node.y is the channel-floor height at the path centre; walls rise
 *  from it by `wall`. The camera flies a little above node.y.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VIDEO_COURSE_H
#define VIDEO_COURSE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COURSE_MAX_NODES  128

/* Material ids (match the renderer's palette banks). */
#define COURSE_MAT_ROCK   0
#define COURSE_MAT_SNOW   1
#define COURSE_MAT_LAVA   2

/* Map defaults: solid rock outside the channel; "open" ceiling = sky. */
#define COURSE_WALL_H     210u
#define COURSE_OPEN_CEIL  255u   /* a ceiling value >= 254 means "open" */

typedef struct {
    float x, z, y;   /* control point: map x/z and centre floor height   */
    float width;     /* corridor half-width                               */
    float wall;      /* how far walls rise above the centre floor         */
    float ceil;      /* tunnel headroom above node.y; <= 0 => open sky     */
    float lava;      /* lava-river half-width down the floor (0 = none)    */
} CourseNode;

typedef struct {
    CourseNode node[COURSE_MAX_NODES];
    int        count;
    bool       loop;     /* true = closed loop, false = linear (start->end) */
} CourseDef;

/* Parameter length: count-1 for an open course, count for a loop. */
float course_length(const CourseDef *c);

/* Catmull-Rom sample of ALL fields at param s (node-index units:
 * 0..count-1 open, wraps for loop). Used by the baker (dense walk) and
 * the camera (path-follow). */
void course_sample(const CourseDef *c, float s, CourseNode *out);

/* Bake the course into floor/ceiling/material maps (mapsz x mapsz,
 * uint8; mapsz must be a power of two — coords wrap). */
void course_bake(const CourseDef *c, uint8_t *floor, uint8_t *ceiling,
                 uint8_t *material, int mapsz);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_COURSE_H */
