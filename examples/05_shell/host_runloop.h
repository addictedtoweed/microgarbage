/* ============================================================
 *  host_runloop.h — multi-session TCP run loop for the shell host.
 *
 *  Setup -> run-until-stop -> teardown. Caller owns the state struct
 *  (HostRunloopTcp) so an MCU build can place it in SDRAM rather than
 *  on the stack.
 *
 *  Each port runs an independent shell session. A slot cycles:
 *
 *    LISTENING (no client)
 *        -> tcp_try_accept succeeds -> spawn shell VM
 *    ACTIVE (shell running)
 *        -> shell exits (vm_sched_get returns NULL after the reap)
 *        -> close client socket, slot returns to LISTENING.
 *
 *  The run loop owns no shutdown of the surrounding VmSystem / audio
 *  service / loaded ELF — those are main()'s, so both the TCP-mode
 *  exit path and the single-session exit path share one cleanup.
 *
 *  Single-session (pty / default stdio) doesn't go through here — it
 *  stays in main() since the shape is small (load one VM, step until
 *  it halts).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_RUNLOOP_H
#define HOST_RUNLOOP_H

#include "host_tcp.h"
#include "host_cli.h"   /* MAX_TCP_PORTS */

#ifdef TCP_MODE_SUPPORTED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vm/vm_system.h"
#include "vm/vm_host_transport.h"
#include "vm/vm_loader.h"   /* VmBacking */

/* Caller-owned table of per-port slot state. Sized at compile time
 * by MAX_TCP_PORTS (from host_tcp.h). n_ports records how many of
 * the slots are live. */
typedef struct {
    TcpCtx          ctxs[MAX_TCP_PORTS];
    VmHostTransport transports[MAX_TCP_PORTS];
    bool            slot_active[MAX_TCP_PORTS];
    uint16_t        slot_vm[MAX_TCP_PORTS];
    int             n_ports;
} HostRunloopTcp;

/* Open N listening sockets and wire the per-port VmHostTransport
 * vtable entries. Prints a connect-hint banner. Returns false if any
 * listener fails — caller exits. */
bool host_runloop_tcp_setup(HostRunloopTcp *rl,
                            const int *ports, int n_ports);

/* Run accept/reap/step until host_platform_stop_requested() flips
 * true (Ctrl-C). Spawns a fresh shell VM per accepted connection
 * out of `elf`/`elf_size`, with `data_kb` of guest data region and
 * `backing` for both text and data XIP backing (the embedded shell
 * case passes the same buffer for both). */
void host_runloop_tcp_run(HostRunloopTcp *rl,
                          VmSystem *sys,
                          const uint8_t *elf, size_t elf_size,
                          uint32_t data_kb,
                          VmBacking backing);

/* Close all client + listen sockets and call tcp_global_shutdown.
 * Idempotent. */
void host_runloop_tcp_teardown(HostRunloopTcp *rl);

#endif /* TCP_MODE_SUPPORTED */

#endif /* HOST_RUNLOOP_H */
