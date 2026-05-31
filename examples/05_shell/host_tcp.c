/* ============================================================
 *  host_tcp.c — TCP transport for the shell host.
 *
 *  See host_tcp.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_tcp.h"

#include <stdio.h>
#include <string.h>

/* Telnet command bytes. */
#define TELNET_IAC  255
#define TELNET_SE   240
#define TELNET_SB   250
#define TELNET_WILL 251
#define TELNET_WONT 252
#define TELNET_DO   253
#define TELNET_DONT 254

/* Send a raw reply on the client socket (best-effort). Used to
 * answer Telnet negotiation. */
static void tcp_raw_send(tcp_sock_t cfd, const void *p, int n) {
    if (cfd == TCP_SOCK_INVALID) return;
#if defined(_WIN32)
    (void)send(cfd, (const char *)p, n, 0);
#else
    (void)send(cfd, p, (size_t)n, 0);
#endif
}

/* Filter Telnet IAC sequences out of `raw` (n bytes), writing the
 * surviving data bytes to `out`. Returns the number of data bytes
 * written. Answers WILL/DO negotiation with WONT/DONT on `cfd` so
 * the client settles. State persists across calls via ctx (IAC
 * sequences can split across reads). A literal 0xFF byte arrives
 * as IAC IAC and is passed through as a single 0xFF. */
static int telnet_filter(TcpCtx *ctx, tcp_sock_t cfd,
                         const unsigned char *raw, int n,
                         unsigned char *out) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char b = raw[i];
        switch (ctx->iac_state) {
        case 0:
            if (b == TELNET_IAC) ctx->iac_state = 1;
            else                 out[w++] = b;
            break;
        case 1:
            if (b == TELNET_IAC) { out[w++] = 0xFF; ctx->iac_state = 0; }
            else if (b == TELNET_WILL || b == TELNET_WONT ||
                     b == TELNET_DO   || b == TELNET_DONT) {
                ctx->iac_verb = b; ctx->iac_state = 2;
            } else if (b == TELNET_SB) {
                ctx->iac_state = 3;
            } else {
                ctx->iac_state = 0;
            }
            break;
        case 2: {
            uint8_t resp[3] = { TELNET_IAC, 0, b };
            if (ctx->iac_verb == TELNET_DO)        resp[1] = TELNET_WONT;
            else if (ctx->iac_verb == TELNET_WILL) resp[1] = TELNET_DONT;
            else resp[1] = 0;
            if (resp[1]) tcp_raw_send(cfd, resp, 3);
            ctx->iac_state = 0;
            break;
        }
        case 3:
            if (b == TELNET_IAC) ctx->iac_state = 4;
            break;
        case 4:
            if (b == TELNET_SE)       ctx->iac_state = 0;
            else if (b == TELNET_IAC) ctx->iac_state = 4;
            else                      ctx->iac_state = 3;
            break;
        }
    }
    return w;
}

int tcp_t_read(VmHostTransport *t, void *buf, unsigned cap) {
    TcpCtx *ctx = (TcpCtx *)t->ctx;
    tcp_sock_t cfd = ctx ? ctx->client_fd : TCP_SOCK_INVALID;
    if (cfd == TCP_SOCK_INVALID) return -5;
    if (cap == 0) return 0;

    /* Read into scratch, then strip Telnet IAC into the caller's
     * buffer. The filter never grows the data, so `cap` bytes of
     * scratch always suffice. */
    unsigned char scratch[512];
    unsigned want = cap < sizeof(scratch) ? cap : (unsigned)sizeof(scratch);
#if defined(_WIN32)
    int r = recv(cfd, (char *)scratch, (int)want, 0);
#else
    /* MSG_DONTWAIT forces a non-blocking read on THIS call. A blocking
     * recv would freeze the entire host (the run loop can't return to
     * check g_stop). */
    ssize_t r = recv(cfd, scratch, want, MSG_DONTWAIT);
#endif
    if (r > 0) {
        /* Entirely-Telnet-negotiation reads yield 0 from the filter
         * — that's "no data this poll," not EOF. */
        return telnet_filter(ctx, cfd, scratch, (int)r, (unsigned char *)buf);
    }
    if (r == 0) {
        /* recv() == 0 on a non-blocking socket means the peer did
         * an orderly shutdown. Report EOF (-1) so the guest's read
         * sees end-of-input and the shell exits, freeing the slot. */
        return -1;
    }
    int err = tcp_last_errno();
    if (err == TCP_WOULDBLOCK) return 0;   /* no data right now */
#if !defined(_WIN32)
    if (err == EINTR) return 0;
#endif
    return -5;
}

int tcp_t_write(VmHostTransport *t, const void *buf, unsigned n) {
    TcpCtx *ctx = (TcpCtx *)t->ctx;
    tcp_sock_t cfd = ctx ? ctx->client_fd : TCP_SOCK_INVALID;
    if (cfd == TCP_SOCK_INVALID) return -5;
    const char *p = (const char *)buf;
    unsigned total_in = 0;
    size_t run_start = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n' && !ctx->prev_was_cr) {
            if (i > run_start) {
#if defined(_WIN32)
                int w = send(cfd, p + run_start, (int)(i - run_start), 0);
#else
                ssize_t w = send(cfd, p + run_start, i - run_start, 0);
#endif
                if (w < 0 && tcp_last_errno() != TCP_WOULDBLOCK) return -5;
            }
#if defined(_WIN32)
            int w2 = send(cfd, "\r\n", 2, 0);
#else
            ssize_t w2 = send(cfd, "\r\n", 2, 0);
#endif
            if (w2 < 0 && tcp_last_errno() != TCP_WOULDBLOCK) return -5;
            total_in += 1;
            run_start = i + 1;
            ctx->prev_was_cr = 0;
            continue;
        }
        ctx->prev_was_cr = (c == '\r');
    }
    if (run_start < n) {
#if defined(_WIN32)
        int w = send(cfd, p + run_start, (int)(n - run_start), 0);
#else
        ssize_t w = send(cfd, p + run_start, n - run_start, 0);
#endif
        if (w < 0 && tcp_last_errno() != TCP_WOULDBLOCK) return -5;
        total_in += (unsigned)(n - run_start);
    }
    return (int)total_in;
}

int tcp_t_flush(VmHostTransport *t) { (void)t; return 0; }

int tcp_t_set_raw(VmHostTransport *t, bool enable) {
    (void)t; (void)enable;
    return 0;
}

void tcp_t_close(VmHostTransport *t) {
    TcpCtx *c = (TcpCtx *)t->ctx;
    if (!c) return;
    if (c->client_fd != TCP_SOCK_INVALID) {
        tcp_close(c->client_fd);
        c->client_fd = TCP_SOCK_INVALID;
    }
    if (c->listen_fd != TCP_SOCK_INVALID) {
        tcp_close(c->listen_fd);
        c->listen_fd = TCP_SOCK_INVALID;
    }
}

/* Set the given socket to non-blocking mode. */
static int tcp_set_nonblock(tcp_sock_t s) {
#if defined(_WIN32)
    u_long mode = 1;
    return (ioctlsocket(s, FIONBIO, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    return (fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0) ? 0 : -1;
#endif
}

#if defined(_WIN32)
static bool g_wsa_started = false;
#endif

tcp_sock_t tcp_listen(int port) {
#if defined(_WIN32)
    if (!g_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "host: WSAStartup failed\n");
            return TCP_SOCK_INVALID;
        }
        g_wsa_started = true;
    }
#endif
    tcp_sock_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == TCP_SOCK_INVALID) {
        fprintf(stderr, "host: socket() failed\n");
        return TCP_SOCK_INVALID;
    }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == TCP_SOCK_ERROR) {
        fprintf(stderr, "host: bind(port=%d) failed\n", port);
        tcp_close(lfd);
        return TCP_SOCK_INVALID;
    }
    if (listen(lfd, 1) == TCP_SOCK_ERROR) {
        fprintf(stderr, "host: listen() failed\n");
        tcp_close(lfd);
        return TCP_SOCK_INVALID;
    }
    /* Non-blocking accept so the main loop can poll N listeners. */
    tcp_set_nonblock(lfd);
    return lfd;
}

void tcp_global_shutdown(void) {
#if defined(_WIN32)
    if (g_wsa_started) {
        WSACleanup();
        g_wsa_started = false;
    }
#endif
}

bool tcp_try_accept(TcpCtx *c) {
    if (c->client_fd != TCP_SOCK_INVALID) return false;  /* already have one */
    struct sockaddr_in cli;
#if defined(_WIN32)
    int cli_len = sizeof(cli);
#else
    socklen_t cli_len = sizeof(cli);
#endif
    tcp_sock_t cfd = accept(c->listen_fd, (struct sockaddr *)&cli, &cli_len);
    if (cfd == TCP_SOCK_INVALID) return false;   /* EWOULDBLOCK = no client yet */

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
               (const char *)&nodelay, sizeof(nodelay));
    tcp_set_nonblock(cfd);
    c->client_fd = cfd;
    fprintf(stderr, "host: [:%d] client connected from %s:%d\n",
            c->port, inet_ntoa(cli.sin_addr), (int)ntohs(cli.sin_port));
    fflush(stderr);
    return true;
}
