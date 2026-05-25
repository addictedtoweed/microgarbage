/* ============================================================
 *  vm_host_transport.h — pluggable I/O transports for guest VMs
 *
 *  A transport is the host-side mechanism by which a guest's
 *  console-style I/O reaches the user (and vice versa). The
 *  base abstraction is byte-stream: read N bytes non-blocking,
 *  write N bytes, flush, optionally toggle raw mode.
 *
 *  All console-style I/O paths funnel through this interface:
 *
 *    SYS_READ(fd=0)     → transport->read_nonblock
 *    SYS_WRITE(fd=1,2)  → transport->write
 *    SYS_FFLUSH(1,2)    → transport->flush
 *    TUI canvas output  → transport->write
 *    TUI input parser   → transport->read_nonblock
 *    set raw mode       → transport->set_raw
 *
 *  Transports are per-session: a host accepting multiple TCP
 *  clients runs an independent shell per connection — each with
 *  its own screen, raw-mode state, and input parser.
 *
 *  Built-in transports (05_shell host.c provides):
 *    - stdio (default, file-descriptors 0/1/2)
 *    - TCP socket (all platforms; repeatable, multi-session)
 *    - POSIX pty (Linux/Cygwin)
 *
 *  Future transports:
 *    - STM32 UART (when hardware bring-up begins)
 *    - lwIP TCP socket on STM32 with 10M Ethernet
 *
 *  Each transport implementation provides a VmHostTransport
 *  vtable. Hosts call vm_host_set_transport() to make it
 *  active. Passing NULL restores the default stdio behavior.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_HOST_TRANSPORT_H
#define VM_HOST_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward decl — defined below. */
struct VmHostTransport;

/* Non-blocking read of up to `cap` bytes. Returns:
 *
 *    > 0   bytes read
 *    = 0   no data available right now
 *    < 0   -errno on error
 *
 * Implementations MUST NOT block. Spinning callers (e.g., the
 * guest's read+yield loop) rely on quick negative responses. */
typedef int (*VmHostTransportReadFn)(struct VmHostTransport *t,
                                     void *buf, unsigned cap);

/* Write `n` bytes from `buf`. Returns:
 *
 *    >= 0  bytes written (may be less than n on partial write)
 *    < 0   -errno on error
 *
 * Implementations MAY block briefly for flow control, but
 * SHOULD NOT block indefinitely. */
typedef int (*VmHostTransportWriteFn)(struct VmHostTransport *t,
                                      const void *buf, unsigned n);

/* Flush any buffered output to its sink. Returns 0 on success
 * or -errno. May be a no-op for transports that don't buffer
 * (e.g., a named pipe with unbuffered WriteFile). */
typedef int (*VmHostTransportFlushFn)(struct VmHostTransport *t);

/* Toggle raw mode on the underlying terminal surface. Returns 0
 * on success or -errno. May be a no-op for transports without
 * terminal semantics (file output, network sockets where the
 * far end is responsible for its own line discipline). */
typedef int (*VmHostTransportSetRawFn)(struct VmHostTransport *t,
                                       bool enable);

/* Optional teardown. Called when the transport is being
 * replaced or the host is shutting down. May be NULL. */
typedef void (*VmHostTransportCloseFn)(struct VmHostTransport *t);

/* Transport vtable + state. Implementations typically embed
 * this in a larger struct that holds their context, OR allocate
 * VmHostTransport with `ctx` pointing to their state.
 *
 * The host module that owns the transport is responsible for
 * the struct's lifetime. */
typedef struct VmHostTransport {
    VmHostTransportReadFn   read_nonblock;
    VmHostTransportWriteFn  write;
    VmHostTransportFlushFn  flush;
    VmHostTransportSetRawFn set_raw;
    VmHostTransportCloseFn  close;

    /* Whether this transport's far end is a real terminal that
     * supports ANSI escapes and cursor positioning. The TUI
     * service uses this to decide whether emitting alt-screen
     * sequences makes sense. */
    bool is_terminal;

    /* Free for the transport implementation to use. */
    void *ctx;
} VmHostTransport;

/* ============================================================
 *  Active transport
 *
 *  The default (process-global) transport is set via
 *  vm_host_set_transport. Most single-session demos use only this.
 *
 *  Round U.6 added per-VM bindings so multi-session hosts can give
 *  each shell VM its own transport. Lookup precedence at every I/O
 *  site is:
 *
 *      1. per-VM binding (vm_host_set_transport_for_vm)
 *      2. process default (vm_host_set_transport)
 *      3. NULL — handler falls through to legacy stdio path
 *
 *  Returns the previous value when setting (NULL if none was
 *  installed). The caller owns the transport struct's lifetime —
 *  setting a new one doesn't free the old one.
 * ============================================================ */
VmHostTransport *vm_host_set_transport(VmHostTransport *t);
VmHostTransport *vm_host_get_transport(void);

/* Per-VM transport binding (round U.6). Pass NULL for `t` to
 * unbind a VM so it falls back to the process default. */
VmHostTransport *vm_host_set_transport_for_vm(uint16_t vm_id,
                                              VmHostTransport *t);
VmHostTransport *vm_host_get_transport_for_vm(uint16_t vm_id);

#endif  /* VM_HOST_TRANSPORT_H */
