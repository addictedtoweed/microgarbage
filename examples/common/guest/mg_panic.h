/* ============================================================
 *  mg_panic.h — error path for cart-side games.
 *
 *  Calling mg_panic captures the message + caller context into
 *  persistent coprocessor RAM, triggers a SNES reset via the
 *  CIC line, and the runtime reboots into its baked-in error.elf
 *  showing the message. The calling VM is halted as part of the
 *  reset sequence; this call does not return.
 *
 *  See docs/game-api.md for the full panic flow.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_PANIC_H
#define MG_PANIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* MgResult — return type for every DMA-bearing mg_* call. */
typedef enum {
    MG_OK            =  0,
    MG_ERR_DMA_SLOTS = -1,  /* 8-slot frame DMA list is full this frame */
    MG_ERR_DMA_BYTES = -2,  /* would blow the per-frame byte budget     */
    MG_ERR_INVALID   = -3,  /* bad argument                             */
} MgResult;

/* Halt the calling VM and reboot the SNES into the runtime's
 * error.elf. Does not return.
 *
 * The message string must remain valid through the call; the runtime
 * captures its contents into persistent coprocessor RAM before the
 * SNES reset fires. */
__attribute__((noreturn))
void mg_panic(const char *msg);

/* Convenience: assert a DMA-bearing call returned MG_OK or panic.
 * The expression is stringified into the panic message so a glance
 * at the error screen identifies the offending call site. */
#define MG_OR_PANIC(expr) \
    do { MgResult _mg_r = (expr); \
         if (_mg_r != MG_OK) mg_panic("DMA failed: " #expr); } while (0)

#ifdef __cplusplus
}
#endif

#endif /* MG_PANIC_H */
