/* ============================================================
 *  tcp_listen.h — PuTTY-friendly TCP transport for the shell VM.
 *
 *  Stands up a single TCP listen socket on a configurable port,
 *  accepts one client at a time, and routes the bytes through the
 *  VmHostTransport vtable bound to the shell VM. The accept loop
 *  is polled from mgapi_step — no threads.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_TCP_LISTEN_H
#define MGAPI_TCP_LISTEN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a listening socket on `port`. Returns 0 on success, negative
 * errno on failure. `shell_vm_id` is the VM ID whose transport we
 * bind to once a client connects. Idempotent: if already listening
 * on this port, returns 0 immediately. */
int mgapi_tcp_listen_init(uint16_t port, uint16_t shell_vm_id);

/* Drop the listen socket and any active client. Idempotent. */
void mgapi_tcp_listen_shutdown(void);

/* Drive the accept loop one tick: check for a pending connection,
 * accept if one arrived, bind transport, etc. Cheap to call
 * frequently; mgapi_step calls this once per emulator frame. */
void mgapi_tcp_listen_poll(void);

/* Dev: client connected? Used by the host test (optional). */
bool mgapi_tcp_listen_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_TCP_LISTEN_H */
