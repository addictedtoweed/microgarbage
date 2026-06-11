/* ============================================================
 *  mg_stream.c — see mg_stream.h
 *
 *  Internals: each handle is encoded as a 16-bit fd in the low
 *  half + a 16-bit arbiter-handle in the high half + a flag bit
 *  in the top to remember "we own the fd, close it on close."
 *  Since the underlying ecall ranges are tiny (fds < 1024,
 *  arbiter slots < 4), packing them into a single int keeps the
 *  guest API as a flat handle without an extra struct.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_stream.h"

#include "fs.h"
#include "vm_runtime.h"

/* Retry interval when the arbiter ring is briefly empty. The
 * arbiter ticks once per host vblank (~16.7 ms); 4 ms is short
 * enough that one tick of latency at most adds one sleep before
 * we get the chunk. */
#define MG_STREAM_RETRY_MS  4u

/* Encoding bits. Negative handles signal invalid; positive
 * handles pack:
 *   bit 31      : OWNS_FD (we opened the fd, must close on close)
 *   bits 30..16 : fd     (0..32767)
 *   bits 15..0  : arbiter handle (0..65535; really 0..3)
 *   The packing fits an int return type without negative values
 *   getting confused with MG_STREAM_INVALID (-1). */
#define MG_OWNS_FD_BIT  (1 << 30)

static inline int pack_handle(int fd, int arb, bool owns_fd) {
    int h = (fd << 16) | (arb & 0xFFFF);
    if (owns_fd) h |= MG_OWNS_FD_BIT;
    return h;
}
static inline int  unpack_fd  (int h) { return (h >> 16) & 0x7FFF; }
static inline int  unpack_arb (int h) { return h & 0xFFFF; }
static inline bool unpack_owns(int h) { return (h & MG_OWNS_FD_BIT) != 0; }

/* Direct ecall wrappers. */
static inline int _sys_stream_register(int fd, uint32_t chunk, uint32_t depth) {
    return (int)_vm_sys3(SYS_STREAM_REGISTER,
                        (uint32_t)fd, chunk, depth);
}
static inline int _sys_stream_consume(int arb, void *dst, uint32_t cap) {
    return (int)_vm_sys3(SYS_STREAM_CONSUME,
                        (uint32_t)arb,
                        (uint32_t)(unsigned long)dst,
                        cap);
}
static inline void _sys_stream_close(int arb) {
    (void)_vm_sys1(SYS_STREAM_CLOSE, (uint32_t)arb);
}
static inline bool _sys_stream_eof(int arb) {
    return _vm_sys1(SYS_STREAM_EOF, (uint32_t)arb) != 0u;
}

/* SYS_SLEEP_TICKS direct wrapper — we don't want to drag in all
 * of mg_input.h just for sleep. One-time-per-handle is too few
 * to inline meaningfully; let the assembler emit the four
 * instructions. */
#ifndef SYS_SLEEP_TICKS
#define SYS_SLEEP_TICKS  1045
#endif
static inline void _sleep_ticks(uint32_t n) {
    (void)_vm_sys1(SYS_SLEEP_TICKS, n);
}

MgStream mg_stream_open_fd(int fd, uint32_t chunk_bytes, uint32_t depth) {
    if (fd < 0) return MG_STREAM_INVALID;
    int arb = _sys_stream_register(fd, chunk_bytes, depth);
    if (arb < 0) return MG_STREAM_INVALID;
    return pack_handle(fd, arb, /*owns_fd=*/false);
}

MgStream mg_stream_open(const char *path, uint32_t chunk_bytes, uint32_t depth) {
    int fd = fs_open(path, O_RDONLY);
    if (fd < 0) return MG_STREAM_INVALID;
    int arb = _sys_stream_register(fd, chunk_bytes, depth);
    if (arb < 0) {
        (void)fs_close(fd);
        return MG_STREAM_INVALID;
    }
    return pack_handle(fd, arb, /*owns_fd=*/true);
}

bool mg_stream_consume(MgStream s, void *dst, uint32_t dst_cap) {
    if (s < 0) return false;
    int arb = unpack_arb(s);

    for (;;) {
        int r = _sys_stream_consume(arb, dst, dst_cap);
        if (r > 0) return true;            /* got a chunk             */
        if (r < 0) return false;           /* -1 = EOF; errno = error */
        /* r == 0: ring is briefly empty. Sleep a bit so the host
         * arbiter tick can refill, then retry. We check EOF after
         * the sleep too in case the producer drains during it. */
        _sleep_ticks(MG_STREAM_RETRY_MS);
    }
}

bool mg_stream_eof(MgStream s) {
    if (s < 0) return true;
    return _sys_stream_eof(unpack_arb(s));
}

void mg_stream_close(MgStream s) {
    if (s < 0) return;
    _sys_stream_close(unpack_arb(s));
    if (unpack_owns(s)) {
        (void)fs_close(unpack_fd(s));
    }
}
