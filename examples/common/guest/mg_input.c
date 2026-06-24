/* ============================================================
 *  mg_input.c — guest-side pad read + edge detection.
 *  See mg_input.h for the contract.
 *
 *  The runtime publishes the producer snapshot via the existing
 *  SYS_COPRO_READ_PADS (1183): the guest passes a 4×u16 buffer and
 *  the runtime fills it with the latest sampled pad words.
 *
 *  Edge detection (pressed / released) is purely guest-side: we
 *  cache the previous frame's bits in static storage and diff each
 *  query. The runtime stays out of per-VM edge bookkeeping — game.elf
 *  and a spawned child.elf each have their own static state, no
 *  collisions.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_input.h"
#include "vm_runtime.h"

/* The bits the caller saw at the most-recent mg_pads(). Compared
 * against the *next* mg_pads return to spot edges. */
static uint16_t s_prev_bits[4] = {0, 0, 0, 0};

/* The bits we returned this call — copied into s_prev_bits at the
 * TOP of the next mg_pads(). Keeps the "previous" snapshot stable
 * for the entire frame the caller spends querying edges. */
static uint16_t s_curr_bits[4] = {0, 0, 0, 0};

MgPads mg_pads(void) {
    /* Promote what the caller had last frame to "previous". */
    s_prev_bits[0] = s_curr_bits[0];
    s_prev_bits[1] = s_curr_bits[1];
    s_prev_bits[2] = s_curr_bits[2];
    s_prev_bits[3] = s_curr_bits[3];

    /* Fetch the runtime's latest published pad snapshot. */
    uint16_t buf[4];
    (void)_vm_sys1(SYS_COPRO_READ_PADS, (uint32_t)buf);
    s_curr_bits[0] = buf[0];
    s_curr_bits[1] = buf[1];
    s_curr_bits[2] = buf[2];
    s_curr_bits[3] = buf[3];

    MgPads out;
    out.p0.bits = buf[0]; out.p0.index = 0;
    out.p1.bits = buf[1]; out.p1.index = 1;
    out.p2.bits = buf[2]; out.p2.index = 2;
    out.p3.bits = buf[3]; out.p3.index = 3;
    return out;
}

bool mg_pad_held(MgPad p, uint16_t btn) {
    return (p.bits & btn) != 0;
}

bool mg_pad_pressed(MgPad p, uint16_t btn) {
    if (p.index >= 4) return false;
    return (p.bits & btn) && !(s_prev_bits[p.index] & btn);
}

bool mg_pad_released(MgPad p, uint16_t btn) {
    if (p.index >= 4) return false;
    return !(p.bits & btn) && (s_prev_bits[p.index] & btn);
}

MgMouse mg_mouse(void) {
    uint32_t packed = _vm_sys0(SYS_MG_READ_MOUSE);
    MgMouse m;
    m.buttons = (uint8_t)(packed & 0xFFu);
    m.dx      = (int8_t)((packed >> 8)  & 0xFFu);
    m.dy      = (int8_t)((packed >> 16) & 0xFFu);
    return m;
}
