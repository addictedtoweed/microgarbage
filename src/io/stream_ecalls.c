/* ============================================================
 *  stream_ecalls.c — SYS_STREAM_REGISTER / CONSUME / CLOSE / EOF
 *
 *  Thin handlers over the stream_arbiter. Pointer translation
 *  uses vm_translate_write because the guest side is the
 *  destination (we copy chunk bytes INTO guest memory).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "io/stream_ecalls.h"

#include "io/stream_arbiter.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"

#include <errno.h>

/* SYS_STREAM_REGISTER(fd, chunk_bytes, depth) → handle or -errno */
static void h_stream_register(VmCpu *cpu, void *system) {
    (void)system;
    int      fd          = (int)cpu->regs[VM_REG_A0];
    uint32_t chunk_bytes = cpu->regs[VM_REG_A1];
    uint32_t depth       = cpu->regs[VM_REG_A2];

    StreamHandle h = stream_arbiter_register(fd, chunk_bytes, depth);
    if (h == STREAM_HANDLE_INVALID) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EINVAL;
        return;
    }
    cpu->regs[VM_REG_A0] = (uint32_t)h;
}

/* SYS_STREAM_CONSUME(handle, dst, dst_cap) → bytes or 0 (empty) or -1 (EOF)
 *
 * Returns chunk_bytes on success (a full chunk copied into dst).
 * Returns 0 if the ring is empty AND not at EOF — guest should
 *   sleep_ticks a small amount and retry.
 * Returns -1 if the producer hit EOF and the ring is drained.
 *
 * dst_cap is a safety check — must be at least chunk_bytes or
 * the call returns -EINVAL. */
static void h_stream_consume(VmCpu *cpu, void *system) {
    (void)system;
    StreamHandle h     = (StreamHandle)(int)cpu->regs[VM_REG_A0];
    uint32_t     dstp  = cpu->regs[VM_REG_A1];
    uint32_t     dcap  = cpu->regs[VM_REG_A2];

    uint32_t chunk = stream_arbiter_chunk_bytes(h);
    if (chunk == 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EBADF;
        return;
    }
    if (dcap < chunk) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EINVAL;
        return;
    }

    /* Validate the guest pointer FIRST so we don't pop a chunk
     * we can't deliver. vm_translate_write checks region bounds
     * + writability. The pop copies directly into guest memory
     * (single memcpy, no host-side scratch). */
    void *dst = vm_translate_write(cpu, dstp, chunk);
    if (!dst) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EFAULT;
        return;
    }

    if (!stream_arbiter_consume(h, dst)) {
        /* Ring empty. dst was untouched (spsc_ring_pop only writes
         * on success). Distinguish EOF from "try again later". */
        if (stream_arbiter_is_eof(h)) {
            cpu->regs[VM_REG_A0] = (uint32_t)-1;   /* sentinel = EOF */
        } else {
            cpu->regs[VM_REG_A0] = 0;              /* try again      */
        }
        return;
    }
    cpu->regs[VM_REG_A0] = chunk;
}

/* SYS_STREAM_CLOSE(handle) → 0 */
static void h_stream_close(VmCpu *cpu, void *system) {
    (void)system;
    StreamHandle h = (StreamHandle)(int)cpu->regs[VM_REG_A0];
    stream_arbiter_unregister(h);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_STREAM_EOF(handle) → 1 if drained-and-EOF, else 0 */
static void h_stream_eof(VmCpu *cpu, void *system) {
    (void)system;
    StreamHandle h = (StreamHandle)(int)cpu->regs[VM_REG_A0];
    cpu->regs[VM_REG_A0] = stream_arbiter_is_eof(h) ? 1u : 0u;
}

/* ----------------------------------------------------------------
 *  Installation
 * ---------------------------------------------------------------- */

bool mgapi_install_stream_ecalls(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;
    VmEcallRouter *r = sys->ecall_router;

    if (!vm_ecall_register(r, SYS_STREAM_REGISTER, h_stream_register)) return false;
    if (!vm_ecall_register(r, SYS_STREAM_CONSUME,  h_stream_consume))  return false;
    if (!vm_ecall_register(r, SYS_STREAM_CLOSE,    h_stream_close))    return false;
    if (!vm_ecall_register(r, SYS_STREAM_EOF,      h_stream_eof))      return false;
    return true;
}
