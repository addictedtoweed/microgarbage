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

#include <stdint.h>

/* Crash context captured at mg_panic time. Matches the host-side
 * MgPanicCtx in src/mgapi/copro_mg_handlers.c — keep both in sync. */
typedef struct {
    uint32_t vm_id;         /* panicking VM id                          */
    uint32_t pc;            /* PC at panic site                          */
    uint32_t a0_a6[7];      /* a0..a6 at panic time                     */
    uint32_t msg_len;       /* bytes available in the message buffer    */
    uint8_t  _reserved[4];
} MgPanicCtx;

/* Read the saved panic message into the caller's buffer. Returns the
 * number of bytes written (always <= cap), or 0 if no panic is
 * pending. The returned bytes are NOT NUL-terminated; caller adds the
 * NUL if it wants C-string semantics.
 *
 * Used by an error-display ELF (or a diagnostic guest) to surface
 * the last mg_panic call. */
uint32_t mg_panic_read   (void *buf, uint32_t cap);

/* Read the crash context (vm_id, PC, a0..a6, message length).
 * Returns MG_OK on success or 0 if no panic is pending. */
MgResult mg_panic_ctx_read(MgPanicCtx *out);

#ifdef __cplusplus
}
#endif

#endif /* MG_PANIC_H */
