/* ============================================================
 *  mg_actor.h — sub-pixel actor wrapping multiple sprite parts.
 *
 *  Pure guest library on top of mg_sprite_*; no ecalls of its own.
 *  An actor owns a Q16.16 sub-pixel position plus zero or more
 *  sprite "parts" — each part is one OAM slot with an (ox, oy)
 *  offset from the actor's origin. Render-time, parts get pushed
 *  to OAM at (actor.x + ox, actor.y + oy) with truncation to pixels.
 *
 *  Deliberately out of scope: animation systems, collision boxes,
 *  sprite-slot allocation, Z-order between actors. Each is too
 *  opinionated to live in the core library; build them on top.
 *
 *  See docs/game-api.md for design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_ACTOR_H
#define MG_ACTOR_H

#include <stdint.h>
#include <stdbool.h>

#include "mg_sprite.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MG_ACTOR_MAX_PARTS
#define MG_ACTOR_MAX_PARTS 8
#endif

/* One sprite part of an actor. (ox, oy) is the offset from the
 * actor's origin in pixels. The part owns a fixed OAM slot. */
typedef struct {
    int16_t  ox, oy;
    uint16_t tile;
    uint8_t  slot;
    uint8_t  palette    : 3;
    uint8_t  priority   : 2;
    uint8_t  hflip      : 1;
    uint8_t  vflip      : 1;
    uint8_t  size_large : 1;
    uint8_t  visible    : 1;
} MgActorPart;

/* All fields public — touch them directly. The library doesn't hide
 * state; this is C, not C++. */
typedef struct {
    int32_t  x_q16, y_q16;     /* sub-pixel position, Q16.16          */
    int32_t  vx_q16, vy_q16;   /* velocity per frame, Q16.16          */
    uint8_t  part_count;
    bool     visible;          /* whole-actor hide                    */
    bool     facing_left;      /* mirrors hflip + ox for all parts    */
    MgActorPart parts[MG_ACTOR_MAX_PARTS];
} MgActor;

/* Zero + defaults: visible=true, facing_left=false, part_count=0. */
void mg_actor_init    (MgActor *a);

/* Append a part. Returns false if MG_ACTOR_MAX_PARTS exceeded. */
bool mg_actor_add_part(MgActor *a, MgActorPart part);

/* Apply velocity to position (x += vx, y += vy). Optional — skip
 * if you handle physics yourself. */
void mg_actor_step    (MgActor *a);

/* Push parts to OAM. One mg_sprite_set per visible part,
 * mg_sprite_hide for hidden parts (or whole-actor hidden). Truncates
 * Q16.16 to pixels at write time. */
void mg_actor_render  (const MgActor *a);

/* Set position from integer pixels. */
void mg_actor_set_pos (MgActor *a, int16_t x, int16_t y);

/* Truncate Q16.16 position to integer pixels. */
static inline int16_t mg_actor_x(const MgActor *a) {
    return (int16_t)(a->x_q16 >> 16);
}
static inline int16_t mg_actor_y(const MgActor *a) {
    return (int16_t)(a->y_q16 >> 16);
}

#ifdef __cplusplus
}
#endif

#endif /* MG_ACTOR_H */
