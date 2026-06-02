/* ============================================================
 *  tcp_listen.c — TCP transport for the shell VM (lifted from
 *  examples/05_shell/host.c). PuTTY (Raw or Telnet) connects on
 *  the configured port; bytes flow through the standard
 *  VmHostTransport vtable bound to the shell VM.
 *
 *  Single-port, single-session for stage 4. On disconnect the
 *  client_fd closes and the shell sees EOF on its next read — the
 *  ergonomics of "reconnect after disconnect" are a stage 4.5
 *  enhancement.
 *
 *  Telnet IAC negotiation is stripped so the classic PuTTY
 *  "burst of garbage on connect" doesn't reach the shell.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "tcp_listen.h"

#include "vm/vm_host_transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>
/* -lws2_32 is passed at link time by build-mgapi.ps1. */

typedef SOCKET tcp_sock_t;
#define TCP_SOCK_INVALID  INVALID_SOCKET
#define TCP_SOCK_ERROR    SOCKET_ERROR
#define TCP_WOULDBLOCK    WSAEWOULDBLOCK

static int tcp_last_errno(void) { return WSAGetLastError(); }
static void tcp_close_sock(tcp_sock_t s) { closesocket(s); }

/* ----------------------------------------------------------------
 *  Per-session state + transport vtable
 * ---------------------------------------------------------------- */

typedef struct {
    tcp_sock_t  listen_fd;
    tcp_sock_t  client_fd;
    uint16_t    port;
    uint16_t    shell_vm_id;
    int         prev_was_cr;   /* dedupe \r\n in the writer */

    /* Telnet IAC stripping state — see telnet_filter. */
    int     iac_state;
    uint8_t iac_verb;
} TcpCtx;

static TcpCtx          g_ctx;
static VmHostTransport g_transport;
static int             g_initialized;
static int             g_wsa_started;

/* --- Telnet constants and IAC negotiation filter --- */
#define TELNET_IAC  255
#define TELNET_SE   240
#define TELNET_SB   250
#define TELNET_WILL 251
#define TELNET_WONT 252
#define TELNET_DO   253
#define TELNET_DONT 254

static void tcp_raw_send(tcp_sock_t cfd, const void *p, int n) {
    if (cfd == TCP_SOCK_INVALID) return;
    (void)send(cfd, (const char *)p, n, 0);
}

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

/* --- transport vtable callbacks --- */

static int tcp_t_read(VmHostTransport *t, void *buf, unsigned cap) {
    TcpCtx *ctx = (TcpCtx *)t->ctx;
    tcp_sock_t cfd = ctx ? ctx->client_fd : TCP_SOCK_INVALID;
    if (cfd == TCP_SOCK_INVALID) return 0;   /* no client yet → idle */
    if (cap == 0) return 0;

    unsigned char scratch[512];
    unsigned want = cap < sizeof(scratch) ? cap : (unsigned)sizeof(scratch);
    int r = recv(cfd, (char *)scratch, (int)want, 0);
    if (r > 0) {
        return telnet_filter(ctx, cfd, scratch, r, (unsigned char *)buf);
    }
    if (r == 0) {
        /* Orderly client shutdown. Close our side; shell sees EOF. */
        tcp_close_sock(cfd);
        ctx->client_fd = TCP_SOCK_INVALID;
        return -1;
    }
    int err = tcp_last_errno();
    if (err == TCP_WOULDBLOCK) return 0;
    return -5;
}

static int tcp_t_write(VmHostTransport *t, const void *buf, unsigned n) {
    TcpCtx *ctx = (TcpCtx *)t->ctx;
    tcp_sock_t cfd = ctx ? ctx->client_fd : TCP_SOCK_INVALID;
    if (cfd == TCP_SOCK_INVALID) return (int)n;  /* discard if no client */

    /* Translate solitary \n into \r\n so PuTTY's terminal advances
     * to the start of the next line. The shell's writes use \n only. */
    const char *p = (const char *)buf;
    unsigned total = 0;
    size_t run_start = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n' && !ctx->prev_was_cr) {
            if (i > run_start) {
                int w = send(cfd, p + run_start, (int)(i - run_start), 0);
                if (w < 0 && tcp_last_errno() != TCP_WOULDBLOCK) return -5;
            }
            int w2 = send(cfd, "\r\n", 2, 0);
            if (w2 < 0 && tcp_last_errno() != TCP_WOULDBLOCK) return -5;
            total += 1;
            run_start = i + 1;
            ctx->prev_was_cr = 0;
            continue;
        }
        ctx->prev_was_cr = (c == '\r');
    }
    if (run_start < n) {
        int w = send(cfd, p + run_start, (int)(n - run_start), 0);
        if (w < 0 && tcp_last_errno() != TCP_WOULDBLOCK) return -5;
        total += (unsigned)(n - run_start);
    }
    return (int)total;
}

static int tcp_t_flush(VmHostTransport *t) { (void)t; return 0; }
static int tcp_t_set_raw(VmHostTransport *t, bool e) { (void)t; (void)e; return 0; }
static void tcp_t_close(VmHostTransport *t) {
    TcpCtx *c = (TcpCtx *)t->ctx;
    if (!c) return;
    if (c->client_fd != TCP_SOCK_INVALID) {
        tcp_close_sock(c->client_fd);
        c->client_fd = TCP_SOCK_INVALID;
    }
}

/* ----------------------------------------------------------------
 *  Listen + accept
 * ---------------------------------------------------------------- */

static int tcp_set_nonblock(tcp_sock_t s) {
    u_long mode = 1;
    return (ioctlsocket(s, FIONBIO, &mode) == 0) ? 0 : -1;
}

static tcp_sock_t open_listen(int port) {
    if (!g_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return TCP_SOCK_INVALID;
        g_wsa_started = 1;
    }
    tcp_sock_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == TCP_SOCK_INVALID) return TCP_SOCK_INVALID;
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == TCP_SOCK_ERROR) {
        tcp_close_sock(lfd);
        return TCP_SOCK_INVALID;
    }
    if (listen(lfd, 1) == TCP_SOCK_ERROR) {
        tcp_close_sock(lfd);
        return TCP_SOCK_INVALID;
    }
    tcp_set_nonblock(lfd);
    return lfd;
}

static bool try_accept(void) {
    if (g_ctx.client_fd != TCP_SOCK_INVALID) return false;
    struct sockaddr_in cli;
    int cli_len = sizeof(cli);
    tcp_sock_t cfd = accept(g_ctx.listen_fd, (struct sockaddr *)&cli, &cli_len);
    if (cfd == TCP_SOCK_INVALID) return false;

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
               (const char *)&nodelay, sizeof(nodelay));
    tcp_set_nonblock(cfd);

    g_ctx.client_fd    = cfd;
    g_ctx.prev_was_cr  = 0;
    g_ctx.iac_state    = 0;
    g_ctx.iac_verb     = 0;

    /* Bind this transport to the shell VM. From here on every byte
     * the shell reads or writes flows through our socket. */
    vm_host_set_transport_for_vm(g_ctx.shell_vm_id, &g_transport);

    /* Greet the client directly through the new socket. The shell's
     * own banner + prompt went to host stdout before the bind (the
     * stub-transport had write=NULL, so output fell through), so
     * without this PuTTY would show a blank window until the user
     * presses Enter to coax the shell into re-emitting a prompt.
     * One short line is enough to confirm to the user that the
     * session is alive; pressing Enter once gets the real prompt. */
    static const char greet[] =
        "mgapi shell -- press Enter for prompt, type 'help' for commands\r\n";
    (void)send(cfd, greet, (int)(sizeof greet - 1), 0);

    fprintf(stderr, "mgapi: [tcp:%u] client connected from %s:%d\n",
            (unsigned)g_ctx.port,
            inet_ntoa(cli.sin_addr), (int)ntohs(cli.sin_port));
    fflush(stderr);
    return true;
}

/* ----------------------------------------------------------------
 *  Public API
 * ---------------------------------------------------------------- */

/* Pre-bound stub transport. Bound to the shell BEFORE any PuTTY
 * client connects so the shell's sys_read / sys_write don't fall
 * through to host stdio.
 *
 * Two distinct stalls this prevents:
 *
 *   1. POSIX read() on the host's stdin -- blocks the entire process
 *      on Windows when stdin is a winpty pty (MSYS / Git-Bash /
 *      mintty). The stub's read_nonblock returns 0 ("no data, try
 *      later") so the shell sleep+retries.
 *
 *   2. fwrite() to host stdout when the host is a Windows GUI-
 *      subsystem .exe (bsnes-plus). LoadLibrary'd mgapi.dll inherits
 *      bsnes's stdout handle which, for a GUI app, is often closed
 *      or pointed at NUL -- and fwrite to it can either block or
 *      silently fail in a way that breaks subsequent reads. The
 *      stub's write returns "wrote everything" without actually
 *      touching anything, so the shell's pre-connect banner and
 *      prompt are silently dropped instead of stalling on the
 *      broken handle.
 *
 *   Pre-connect output is no real loss in either case -- the
 *   accept-time greeting (sent later through the real socket)
 *   tells the user the session is up. After accept,
 *   vm_host_set_transport_for_vm replaces this stub with the real
 *   TCP transport and everything flows through PuTTY. */
static int stub_t_read(VmHostTransport *t, void *buf, unsigned cap) {
    (void)t; (void)buf; (void)cap;
    return 0;   /* "no data" -- shell sleeps + retries */
}
static int stub_t_write(VmHostTransport *t, const void *buf, unsigned n) {
    (void)t; (void)buf;
    return (int)n;   /* pretend the bytes went somewhere */
}
static int stub_t_flush(VmHostTransport *t) { (void)t; return 0; }
static VmHostTransport g_stub_transport = {
    .read_nonblock = stub_t_read,
    .write         = stub_t_write,
    .flush         = stub_t_flush,
    .set_raw       = NULL,
    .close         = NULL,
    .is_terminal   = true,
    .ctx           = NULL,
};

int mgapi_tcp_listen_init(uint16_t port, uint16_t shell_vm_id) {
    if (g_initialized) return 0;

    g_ctx.listen_fd   = open_listen(port);
    if (g_ctx.listen_fd == TCP_SOCK_INVALID) return -EADDRINUSE;
    g_ctx.client_fd   = TCP_SOCK_INVALID;
    g_ctx.port        = port;
    g_ctx.shell_vm_id = shell_vm_id;

    g_transport.read_nonblock = tcp_t_read;
    g_transport.write         = tcp_t_write;
    g_transport.flush         = tcp_t_flush;
    g_transport.set_raw       = tcp_t_set_raw;
    g_transport.close         = tcp_t_close;
    g_transport.is_terminal   = true;
    g_transport.ctx           = &g_ctx;

    /* Bind the stub so the shell's first sys_read doesn't fall
     * through to a blocking host-stdin read. Replaced by g_transport
     * on the first PuTTY accept; if PuTTY later disconnects,
     * tcp_t_read goes idle (returns 0) which behaves identically. */
    vm_host_set_transport_for_vm(shell_vm_id, &g_stub_transport);

    g_initialized = 1;
    fprintf(stderr, "mgapi: listening on TCP :%u for shell vm %u\n",
            (unsigned)port, (unsigned)shell_vm_id);
    fflush(stderr);
    return 0;
}

void mgapi_tcp_listen_shutdown(void) {
    if (!g_initialized) return;
    if (g_ctx.client_fd != TCP_SOCK_INVALID) {
        tcp_close_sock(g_ctx.client_fd);
        g_ctx.client_fd = TCP_SOCK_INVALID;
    }
    if (g_ctx.listen_fd != TCP_SOCK_INVALID) {
        tcp_close_sock(g_ctx.listen_fd);
        g_ctx.listen_fd = TCP_SOCK_INVALID;
    }
    g_initialized = 0;
    /* Leave WSAStartup'd — cheap and the embedder may re-init us. */
}

/* Forward decl from vm_init.c -- halts a running spawned VM if any
 * parent is parked on it, so the next reap delivers exit 130 to the
 * shell. Returns the killed child's vm_id, or UINT16_MAX if no
 * spawn is in flight. */
extern uint16_t mgapi_vm_kill_running_spawn(void);

/* While the shell is blocked in sys_spawn_and_wait on a running
 * demo, the demo doesn't read stdin -- so any bytes the user types
 * in PuTTY pile up in the OS socket buffer. We MSG_PEEK that buffer
 * each poll; if a Ctrl-C byte (0x03) is sitting there, halt the
 * running spawn (the reap delivers exit 130 to the shell, the shell
 * resumes its prompt) and consume bytes up to and including the
 * 0x03 so they don't show up on the next command line. The byte
 * stays out of the shell's transport read stream because we drain
 * it here.
 *
 * No-op when no client is connected or no spawn is in flight. */
static void check_for_ctrlc_kill(void) {
    if (g_ctx.client_fd == TCP_SOCK_INVALID) return;
    unsigned char peek[32];
    int r = recv(g_ctx.client_fd, (char *)peek, sizeof(peek),
                 MSG_PEEK);
    if (r <= 0) {
        /* WOULDBLOCK / no data / orderly-close -- ignore here;
         * tcp_t_read handles the disconnect case when the shell's
         * read fires next. */
        return;
    }
    int idx = -1;
    for (int i = 0; i < r; i++) {
        if (peek[i] == 0x03) { idx = i; break; }
    }
    if (idx < 0) return;

    uint16_t killed = mgapi_vm_kill_running_spawn();
    if (killed == (uint16_t)UINT16_MAX) {
        /* No spawn running -- leave the byte alone so the shell sees
         * it via its normal read path (Ctrl-C at the prompt is a
         * line-clear, handled in readline_raw). */
        return;
    }

    /* Drain bytes up to and including the 0x03 so the next shell
     * read starts clean. */
    char drain[32];
    int to_drain = idx + 1;
    while (to_drain > 0) {
        int got = recv(g_ctx.client_fd, drain,
                       to_drain < (int)sizeof(drain) ? to_drain
                                                    : (int)sizeof(drain),
                       0);
        if (got <= 0) break;
        to_drain -= got;
    }

    fprintf(stderr, "mgapi: Ctrl-C -- killed spawn vm %u\n",
            (unsigned)killed);
    fflush(stderr);
}

void mgapi_tcp_listen_poll(void) {
    if (!g_initialized) return;
    (void)try_accept();
    check_for_ctrlc_kill();
}

bool mgapi_tcp_listen_client_connected(void) {
    return g_initialized && g_ctx.client_fd != TCP_SOCK_INVALID;
}
