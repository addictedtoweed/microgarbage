/* ============================================================
 *  mg_input.h — joypad input for cart-side games.
 *
 *  The runtime polls all four joypads as part of its hot path and
 *  publishes a producer snapshot. Guests read the snapshot via
 *  mg_pads(); edge detection (pressed / released) is library-side,
 *  per-VM, so spawned child VMs see independent edges without
 *  colliding with their parent.
 *
 *  Button bits match the SNES auto-joypad word layout exactly, so a
 *  guest that needs the raw $4218 shape just looks at MgPad.bits.
 *
 *  See docs/game-api.md for the producer model.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_INPUT_H
#define MG_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Button bits, layout matches SNES auto-joypad word $4218 (high bit
 * = B, ..., low nibble = L/R/X/A in the standard SNES order). */
enum {
    MG_BTN_B      = 0x8000,
    MG_BTN_Y      = 0x4000,
    MG_BTN_SELECT = 0x2000,
    MG_BTN_START  = 0x1000,
    MG_BTN_UP     = 0x0800,
    MG_BTN_DOWN   = 0x0400,
    MG_BTN_LEFT   = 0x0200,
    MG_BTN_RIGHT  = 0x0100,
    MG_BTN_A      = 0x0080,
    MG_BTN_X      = 0x0040,
    MG_BTN_L      = 0x0020,
    MG_BTN_R      = 0x0010,
};

typedef struct {
    uint16_t bits;     /* MG_BTN_* mask                  */
    uint8_t  index;    /* 0..3, lets edge-detect lookup
                        * find this pad's prev-frame state */
} MgPad;

typedef struct {
    MgPad p0, p1, p2, p3;
} MgPads;

/* Latest producer-published pad snapshot. Non-blocking. Reflects the
 * pads as sampled by the runtime's most-recent mg_wait_frame return. */
MgPads mg_pads(void);

/* Predicates. `held` returns the current frame's state; `pressed`
 * and `released` are edge-detected against this VM's previous-frame
 * snapshot (the library maintains it). */
bool mg_pad_held    (MgPad p, uint16_t btn);
bool mg_pad_pressed (MgPad p, uint16_t btn);
bool mg_pad_released(MgPad p, uint16_t btn);

/* Port-2 SNES Mouse. dx/dy are relative motion since the last read (the
 * runtime accumulates + drains them per call); buttons is the current
 * level. Same hardware the FMV player's cursor/bullet overlay uses, so a
 * guest menu can be driven by clicks with no PuTTY shell. */
enum { MG_MOUSE_LEFT = 0x01, MG_MOUSE_RIGHT = 0x02 };

typedef struct {
    int8_t  dx, dy;
    uint8_t buttons;   /* MG_MOUSE_* mask */
} MgMouse;

MgMouse mg_mouse(void);

#ifdef __cplusplus
}
#endif

#endif /* MG_INPUT_H */
