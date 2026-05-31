/* ============================================================
 *  host_tcp.h — TCP transport for the shell host.
 *
 *  Listens on a TCP port; on the first connection, that socket
 *  becomes the transport's read/write target. Single-session per
 *  port; main() allocates an array of `TcpCtx` (one per --tcp=
 *  port) and polls each via tcp_try_accept() in the run loop.
 *
 *  Telnet IAC negotiation is stripped on read so a PuTTY Telnet
 *  client doesn't dump escape garbage into the shell's input.
 *  Write path translates lone '\n' into '\r\n' so the client
 *  terminal advances to the start of the next line.
 *
 *  Only host.c includes this header — that's why the heavy
 *  <winsock2.h> / <sys/socket.h> live here rather than in a more
 *  shared place.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_TCP_H
#define HOST_TCP_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET tcp_sock_t;
#  define TCP_SOCK_INVALID INVALID_SOCKET
#  define TCP_SOCK_ERROR   SOCKET_ERROR
#  define tcp_close(s)     closesocket(s)
#  define tcp_last_errno() WSAGetLastError()
#  define TCP_WOULDBLOCK   WSAEWOULDBLOCK
#  define TCP_MODE_SUPPORTED 1
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int tcp_sock_t;
#  define TCP_SOCK_INVALID (-1)
#  define TCP_SOCK_ERROR   (-1)
#  define tcp_close(s)     close(s)
#  define tcp_last_errno() errno
#  define TCP_WOULDBLOCK   EAGAIN
#  define TCP_MODE_SUPPORTED 1
#endif

#include "vm/vm_host_transport.h"

/* Per-instance TCP transport state. main() declares one per --tcp=
 * port; the VmHostTransport's ctx pointer points at the matching
 * TcpCtx, so multiple sessions each have their own sockets. */
typedef struct {
    tcp_sock_t listen_fd;
    tcp_sock_t client_fd;
    int        port;
    int        prev_was_cr;   /* LF->CRLF write-side state */

    /* Telnet IAC filter state, per-connection. Many clients open
     * with a burst of IAC negotiation; we don't speak Telnet, but
     * we strip the sequences so they don't reach the shell as
     * garbage and answer WILL/DO with WONT/DONT so the client
     * stops asking.
     *
     * iac_state: 0=ground, 1=saw IAC, 2=saw IAC+verb (await
     * option), 3=inside subnegotiation, 4=subneg saw IAC. */
    int        iac_state;
    uint8_t    iac_verb;
} TcpCtx;

/* Open a listening socket on `port`. Returns the listen fd or
 * TCP_SOCK_INVALID on failure. Does NOT accept; the caller stores
 * the result in TcpCtx::listen_fd and polls with tcp_try_accept. */
tcp_sock_t tcp_listen(int port);

/* Poll the ctx's listener for a pending connection. Returns true
 * if a client just connected (ctx->client_fd now valid). Idempotent
 * once a client is connected — returns false until tcp_t_close
 * tears the session down. */
bool tcp_try_accept(TcpCtx *c);

/* VmHostTransport vtable entries. main() wires these into one
 * VmHostTransport per port; the transport's ctx points at the
 * matching TcpCtx. */
int  tcp_t_read(VmHostTransport *t, void *buf, unsigned cap);
int  tcp_t_write(VmHostTransport *t, const void *buf, unsigned n);
int  tcp_t_flush(VmHostTransport *t);
int  tcp_t_set_raw(VmHostTransport *t, bool enable);
void tcp_t_close(VmHostTransport *t);

/* Process-wide TCP teardown. On Windows, calls WSACleanup() if any
 * tcp_listen() ever fired WSAStartup(). On POSIX, a no-op. Idempotent.
 * main() calls this once at exit; per-socket tcp_t_close handles the
 * per-connection close path. */
void tcp_global_shutdown(void);

#endif /* HOST_TCP_H */
