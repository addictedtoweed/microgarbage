/* ============================================================
 *  mg_actor.c — pure-C actor implementation.
 *  See mg_actor.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_actor.h"

/* memset / no libc dependency: tiny inline replacement. The guest
 * libc port has memset, but mg_actor wants to compile clean even
 * when linked into a minimal game.elf that hasn't pulled libc. */
static inline void mg_zero(void *p, unsigned n) {
    unsigned char *b = (unsigned char *)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

void mg_actor_init(MgActor *a) {
    mg_zero(a, sizeof(*a));
    a->visible = true;
}

bool mg_actor_add_part(MgActor *a, MgActorPart part) {
    if (a->part_count >= MG_ACTOR_MAX_PARTS) return false;
    a->parts[a->part_count++] = part;
    return true;
}

void mg_actor_step(MgActor *a) {
    a->x_q16 += a->vx_q16;
    a->y_q16 += a->vy_q16;
}

void mg_actor_set_pos(MgActor *a, int16_t x, int16_t y) {
    a->x_q16 = (int32_t)x << 16;
    a->y_q16 = (int32_t)y << 16;
}

void mg_actor_render(const MgActor *a) {
    int16_t base_x = mg_actor_x(a);
    int16_t base_y = mg_actor_y(a);
    for (uint8_t i = 0; i < a->part_count; i++) {
        const MgActorPart *p = &a->parts[i];

        /* Whole-actor hide OR per-part hidden: emit mg_sprite_hide
         * so the shadow OAM gets the offscreen Y for this slot. */
        if (!a->visible || !p->visible) {
            mg_sprite_hide(p->slot);
            continue;
        }

        /* Mirror ox + hflip when facing left. The runtime sees one
         * coherent sprite — the renderer never knows the actor
         * orientation. */
        int16_t ox     = a->facing_left ? -p->ox : p->ox;
        uint8_t hflip  = a->facing_left ? (p->hflip ^ 1u) : p->hflip;

        /* Y is uint8 in the OAM, so signed-overflow at the screen
         * edges is the OAM-y=240 hidden idiom. Let it happen. */
        int    sx = (int)base_x + (int)ox;
        int    sy = (int)base_y + (int)p->oy;

        MgSprite s = {
            .x          = (int16_t)sx,
            .y          = (uint8_t)sy,
            .tile       = p->tile,
            .palette    = p->palette,
            .priority   = p->priority,
            .hflip      = hflip,
            .vflip      = p->vflip,
            .size_large = p->size_large,
        };
        mg_sprite_set(p->slot, &s);
    }
}
