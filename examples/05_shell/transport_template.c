/* ============================================================
 *  transport_template.c — copy this to add a new transport
 *
 *  A "transport" is how the host moves bytes to/from a client:
 *  stdio, a named pipe, a pty, TCP, a UART, a WebSocket, whatever.
 *  The VM and shell don't care which — they talk to the
 *  VmHostTransport vtable below.
 *
 *  TO ADD ONE (≈5 minutes):
 *    1. Copy this file to transport_<yours>.c.
 *    2. Put your connection state in MyCtx.
 *    3. Fill in the 5 functions (read/write/flush/set_raw/close).
 *       The read/write contracts are the only thing to get right —
 *       see the comments on each.
 *    4. Call my_transport_setup() from host.c after stdio is
 *       installed, the same way setup_pty_transport() / the TCP
 *       transport setup is wired in.
 *    5. Add the file to the build (build.sh / build-win.*).
 *
 *  This template is a working LOOPBACK: whatever the guest writes,
 *  it can read back. Useful to compile-test the wiring before you
 *  have real I/O. Replace the MyCtx buffer with your real source.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_host_transport.h"
#include "vm/vm_system.h"
#include <string.h>

/* --- 1. Your per-connection state. ----------------------------
 * One instance per connection. The vtable's `ctx` points here, so
 * several connections can each have their own. (The loopback just
 * needs a byte buffer; a real transport holds a socket fd, a HANDLE,
 * a UART peripheral pointer, etc.) */
typedef struct {
    unsigned char buf[256];
    unsigned      len;
} MyCtx;

static MyCtx g_my_ctx;   /* single instance for this demo */

/* --- 2. read_nonblock --- MUST NOT block. ---------------------
 * Return:  >0 = bytes read   0 = nothing right now   <0 = -errno
 * The shell's read loop spins on this, so "no data" must return 0
 * immediately, never wait. */
static int my_read(VmHostTransport *t, void *buf, unsigned cap) {
    MyCtx *c = (MyCtx *)t->ctx;
    if (cap == 0 || c->len == 0) return 0;          /* no data now */
    unsigned n = c->len < cap ? c->len : cap;
    memcpy(buf, c->buf, n);
    memmove(c->buf, c->buf + n, c->len - n);        /* consume */
    c->len -= n;
    return (int)n;                                  /* bytes read */
}

/* --- 3. write --- Return >=0 bytes written, or <0 -errno. ------
 * Most terminal transports translate a lone '\n' to "\r\n" here so
 * a client terminal shows new lines at column 0. The loopback skips
 * that and just stashes the bytes to be read back. */
static int my_write(VmHostTransport *t, const void *buf, unsigned n) {
    MyCtx *c = (MyCtx *)t->ctx;
    unsigned space = (unsigned)sizeof(c->buf) - c->len;
    unsigned w = n < space ? n : space;
    memcpy(c->buf + c->len, buf, w);
    c->len += w;
    return (int)w;                                  /* bytes written */
}

/* --- 4. flush --- push buffered output; 0 ok / -errno. --------
 * No-op if your transport doesn't buffer (sockets, pipes usually
 * don't at this layer). */
static int my_flush(VmHostTransport *t) {
    (void)t;
    return 0;
}

/* --- 5. set_raw --- toggle terminal raw mode; 0 ok / -errno. ---
 * No-op for anything without a line discipline (sockets, pipes,
 * UARTs). Only real terminals/ptys do something here. */
static int my_set_raw(VmHostTransport *t, bool enable) {
    (void)t; (void)enable;
    return 0;
}

/* --- 6. close --- optional teardown; may be left NULL. --------- */
static void my_close(VmHostTransport *t) {
    MyCtx *c = (MyCtx *)t->ctx;
    if (c) c->len = 0;
}

/* The vtable. `is_terminal` = true means "the far end understands
 * ANSI escapes / cursor moves" (so the TUI uses alt-screen, etc.).
 * Set false for a plain byte sink. */
static VmHostTransport g_my_transport = {
    .read_nonblock = my_read,
    .write         = my_write,
    .flush         = my_flush,
    .set_raw       = my_set_raw,
    .close         = my_close,
    .is_terminal   = true,
    .ctx           = &g_my_ctx,
};

/* Call this from host.c (after vm_host_install_stdio_ex) to make
 * the host speak over your transport. For multi-session hosts, bind
 * per-VM with vm_host_set_transport_for_vm(vm_id, &g_my_transport)
 * instead — see host.c's TCP run loop for that pattern. */
bool my_transport_setup(VmSystem *sys) {
    (void)sys;
    g_my_ctx.len = 0;
    vm_host_set_transport(&g_my_transport);
    return true;
}
