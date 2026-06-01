/* ============================================================
 *  mgapi_host_test.c — exercise mgapi.dll without bsnes.
 *
 *  Stage 1 verification: LoadLibrary the DLL the same way the
 *  bsnes-plus mapper will, resolve every exported symbol by name,
 *  initialize the runtime, stage the assembled snes_smoke.sfc into
 *  the cart window, and read cart-bus addresses back to confirm
 *  the decode table behaves as the SNES kernel expects.
 *
 *  Build: build-mgapi.ps1 picks this up automatically if it lives
 *  in tools/.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <windows.h>

/* We DO NOT include mgapi.h directly — the whole point of this test
 * is to LoadLibrary the DLL the way an external embedder (bsnes)
 * does, then resolve symbols by name. The struct / constants are
 * copied here to keep the test free-standing. If the ABI ever drifts
 * the test fails to link and tells us. */

#define MGAPI_CART_WINDOW_BYTES   (64u * 1024u)
#define MGAPI_AUDIO_SAMPLE_RATE_HZ  44100u

typedef struct {
    uint32_t hold_ms;
} MgapiResetConfigT;

typedef struct {
    uint32_t  cart_window_size;
    uint32_t  audio_sample_rate;
    uint32_t  audio_frames_max;
    uint16_t  tcp_listen_port;
    uint8_t   pad_count;
    uint8_t   rom_select;        /* 0 = smoke, 1 = boot, 2 = none */
    const char *shell_elf_path;
    const char *autostart_path;
    MgapiResetConfigT reset;
    uint8_t   disable_default_stdio;
    uint8_t   _reserved_pad[7];
} MgapiConfig;

typedef int      (*pf_init)(const MgapiConfig *);
typedef void     (*pf_shutdown)(void);
typedef uint8_t  (*pf_cart_read)(uint32_t);
typedef void     (*pf_post_pads)(const uint16_t *);
typedef void     (*pf_step)(uint64_t);
typedef uint32_t (*pf_audio_pull)(int16_t *, uint32_t);
typedef const char *(*pf_version)(void);
typedef void     (*pf_dev_load_blob)(uint32_t, const void *, uint32_t);
typedef void     (*pf_dev_pool_sizes)(uint32_t *);
typedef void     (*pf_dev_audio_ring)(uint32_t *);
typedef int      (*pf_dev_cart_stats)(uint32_t *);
typedef void *   (*pf_dev_l2_alloc)(uint32_t, uint32_t);
typedef void     (*pf_dev_l2_free)(void *);
typedef int      (*pf_dev_l2_stats)(uint64_t *);
typedef void     (*pf_dev_vm_stats)(uint32_t *);
typedef int      (*pf_dev_run_l2_test)(void);
typedef int      (*pf_dev_stage_menu)(void);
typedef int      (*pf_dev_spawn_demo)(const char *, uint32_t);
typedef int      (*pf_dev_td0_size)(const char *);
typedef int      (*pf_dev_demo_via_trashfs)(const char *);
typedef void     (*pf_reset_begin)(void);
typedef int      (*pf_reset_ready)(void);
typedef void     (*pf_reset_end)(void);

/* ----------------------------------------------------------------
 *  Tiny test harness — print PASS/FAIL per case, exit non-zero if
 *  anything fails.
 * ---------------------------------------------------------------- */

static int g_fails = 0;
static int g_passes = 0;

/* Ctrl-C flag set by either of the two break-signal paths below.
 * The --tcp loop polls this each tick so the user can break out of
 * the forever-loop without task-killing the process (which would
 * leave the TCP port in TIME_WAIT for a few seconds). Volatile so
 * the loop reliably sees the write. */
static volatile LONG g_ctrlc_seen = 0;

/* Native-Win32 console handler. Fires when the process owns a real
 * console (cmd.exe, PowerShell, Windows Terminal) and the user
 * presses Ctrl-C / Ctrl-Break or closes the window. */
static BOOL WINAPI ctrlc_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_ctrlc_seen, 1);
        /* Returning TRUE tells Windows we handled it; the default
         * handler would otherwise call ExitProcess immediately and
         * skip our shutdown. */
        return TRUE;
    }
    return FALSE;
}

/* POSIX-shaped signal handler. Fires when the process is launched
 * from an MSYS / Cygwin / Git-Bash shell behind a winpty pseudo-
 * terminal — those environments deliver Ctrl-C as SIGINT, not as a
 * console-control event, so the Win32 handler above never sees it. */
#include <signal.h>
static void sigint_handler(int sig) {
    (void)sig;
    InterlockedExchange(&g_ctrlc_seen, 1);
    /* Re-arm — the CRT resets to SIG_DFL after delivering once. */
    signal(SIGINT, sigint_handler);
}

/* Stdin watcher thread — the last-resort Ctrl-C path. MSYS/Git-Bash
 * + mintty often deliver Ctrl-C as raw byte 0x03 (ETX) in stdin
 * rather than as a console-control event or SIGINT. The thread does
 * blocking 1-byte reads and flips g_ctrlc_seen on 0x03, 'q' / 'Q',
 * or EOF (terminal closed). This intentionally consumes stdin
 * bytes — in --tcp mode all VM I/O routes through the TCP
 * transport, so stealing host stdin is harmless. */
static DWORD WINAPI stdin_watcher(LPVOID arg) {
    (void)arg;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return 0;
    for (;;) {
        char c;
        DWORD n = 0;
        if (!ReadFile(h, &c, 1, &n, NULL) || n == 0) {
            /* EOF or pipe closed — treat as quit. */
            InterlockedExchange(&g_ctrlc_seen, 1);
            return 0;
        }
        if (c == 0x03 || c == 'q' || c == 'Q') {
            InterlockedExchange(&g_ctrlc_seen, 1);
            return 0;
        }
    }
}

static void check_eq_u8(const char *what, uint8_t got, uint8_t want) {
    if (got == want) {
        printf("  PASS  %-48s got=0x%02x\n", what, got);
        g_passes++;
    } else {
        printf("  FAIL  %-48s got=0x%02x want=0x%02x\n", what, got, want);
        g_fails++;
    }
}

static int read_file(const char *path, void **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n);
    if (!p) { fclose(f); return -1; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return -1;
    }
    fclose(f);
    *out_buf = p; *out_len = (size_t)n;
    return 0;
}

/* ----------------------------------------------------------------
 *  Main
 * ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* Unbuffer stdout so output isn't lost if the DLL crashes mid-run. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* CLI shape: mgapi_host_test [dll] [sfc] [--tcp <port>]
     *
     * With --tcp <port> the binary runs the usual self-test, then
     * keeps stepping the runtime forever so a PuTTY session can
     * raw-connect to localhost:<port> and drive the shell. Without
     * --tcp it runs the tests and exits (the original behaviour). */
    const char *dll_path = "build\\mgapi\\mgapi.dll";
    const char *sfc_path = "snes\\build\\snes_smoke.sfc";
    uint16_t    tcp_port = 0;
    int pos = 1;
    while (pos < argc) {
        if (strcmp(argv[pos], "--tcp") == 0 && pos + 1 < argc) {
            int p = atoi(argv[pos + 1]);
            if (p > 0 && p < 65536) tcp_port = (uint16_t)p;
            pos += 2;
        } else if (argv[pos][0] != '-') {
            if (pos == 1) dll_path = argv[pos];
            else if (pos == 2) sfc_path = argv[pos];
            pos++;
        } else {
            pos++;   /* unknown -flag, skip */
        }
    }

    printf("mgapi_host_test\n");
    printf("  dll: %s\n  sfc: %s\n\n", dll_path, sfc_path);

    HMODULE m = LoadLibraryA(dll_path);
    if (!m) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }

    /* GetProcAddress returns FARPROC (a generic function pointer of
     * unspecified-arg shape). Casting it to a specific signature is
     * exactly the API's contract, but -Wpedantic and -Wcast-function-
     * type both complain. The (void*) hop fixes pedantic, and we
     * silence cast-function-type locally — this is the canonical
     * Win32 idiom; the warning is structurally inapplicable here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wpedantic"
    pf_init           p_init = (pf_init)         GetProcAddress(m, "mgapi_init");
    pf_shutdown       p_shut = (pf_shutdown)     GetProcAddress(m, "mgapi_shutdown");
    pf_cart_read      p_read = (pf_cart_read)    GetProcAddress(m, "mgapi_cart_read");
    pf_post_pads      p_pads = (pf_post_pads)    GetProcAddress(m, "mgapi_post_joypads");
    pf_step           p_step = (pf_step)         GetProcAddress(m, "mgapi_step");
    pf_audio_pull     p_pull = (pf_audio_pull)   GetProcAddress(m, "mgapi_audio_pull");
    pf_version        p_ver  = (pf_version)      GetProcAddress(m, "mgapi_version");
    pf_dev_load_blob  p_load = (pf_dev_load_blob)GetProcAddress(m, "mgapi_dev_load_blob");
    pf_dev_pool_sizes p_szs  = (pf_dev_pool_sizes)GetProcAddress(m, "mgapi_dev_pool_sizes");
    pf_dev_audio_ring p_ring = (pf_dev_audio_ring)GetProcAddress(m, "mgapi_dev_audio_ring");
    pf_dev_cart_stats p_cart = (pf_dev_cart_stats)GetProcAddress(m, "mgapi_dev_cart_stats");
    pf_dev_l2_alloc   p_l2a  = (pf_dev_l2_alloc)  GetProcAddress(m, "mgapi_dev_l2_alloc");
    pf_dev_l2_free    p_l2f  = (pf_dev_l2_free)   GetProcAddress(m, "mgapi_dev_l2_free");
    pf_dev_l2_stats   p_l2s  = (pf_dev_l2_stats)  GetProcAddress(m, "mgapi_dev_l2_stats");
    pf_dev_vm_stats   p_vm   = (pf_dev_vm_stats)  GetProcAddress(m, "mgapi_dev_vm_stats");
    pf_dev_run_l2_test p_l2t = (pf_dev_run_l2_test)GetProcAddress(m, "mgapi_dev_run_l2_test");
    pf_dev_stage_menu  p_men = (pf_dev_stage_menu) GetProcAddress(m, "mgapi_dev_stage_menu_one_frame");
    pf_dev_spawn_demo  p_dem = (pf_dev_spawn_demo) GetProcAddress(m, "mgapi_dev_spawn_demo_for_steps");
    pf_dev_td0_size    p_td0 = (pf_dev_td0_size)   GetProcAddress(m, "mgapi_dev_td0_demo_size");
    pf_dev_demo_via_trashfs p_dtf = (pf_dev_demo_via_trashfs)
                                    GetProcAddress(m, "mgapi_dev_spawn_demo_via_trashfs");
    pf_reset_begin     p_rsb = (pf_reset_begin)    GetProcAddress(m, "mgapi_cart_reset_begin");
    pf_reset_ready     p_rsr = (pf_reset_ready)    GetProcAddress(m, "mgapi_cart_reset_ready");
    pf_reset_end       p_rse = (pf_reset_end)      GetProcAddress(m, "mgapi_cart_reset_end");
#pragma GCC diagnostic pop

    if (!p_init || !p_shut || !p_read || !p_pads || !p_step ||
        !p_pull || !p_ver || !p_load || !p_szs || !p_ring || !p_cart ||
        !p_l2a || !p_l2f || !p_l2s || !p_vm || !p_l2t ||
        !p_rsb || !p_rsr || !p_rse || !p_men) {
        printf("GetProcAddress missing one or more symbols\n");
        return 3;
    }

    printf("DLL version: %s\n\n", p_ver());

    MgapiConfig cfg = {
        .cart_window_size  = MGAPI_CART_WINDOW_BYTES,
        .audio_sample_rate = MGAPI_AUDIO_SAMPLE_RATE_HZ,
        .audio_frames_max  = 4096,
        .tcp_listen_port   = tcp_port,
        .pad_count         = 2,
        .rom_select        = 0,   /* smoke ROM */
        .shell_elf_path    = NULL,
        .autostart_path    = NULL,
        .reset             = { .hold_ms = 20 },  /* short hold for tests */
    };

    printf("calling mgapi_init...\n");
    int rc = p_init(&cfg);
    printf("mgapi_init returned %d\n", rc);
    if (rc != 0) {
        printf("mgapi_init failed: %d\n", rc);
        return 4;
    }

    /* /cart/ trashfs volume — stage 2c. Format+mount succeeded if
     * stats come back sane (free <= total, both > 0, inodes available). */
    printf("--- /cart/ trashfs volume on the 1 MB PSRAM slice ---\n");
    {
        uint32_t st[4] = {0,0,0,0};
        int r = p_cart(st);
        printf("  total_blocks=%u free=%u  inodes=%u free=%u  (rc=%d)\n",
               st[0], st[1], st[2], st[3], r);
        if (r == 0) g_passes++;
        else { printf("  FAIL  cart stats rc != 0\n"); g_fails++; }
        if (st[0] > 0 && st[1] > 0 && st[1] <= st[0]) g_passes++;
        else { printf("  FAIL  total/free blocks not sane\n"); g_fails++; }
        if (st[2] > 0 && st[3] > 0 && st[3] <= st[2]) g_passes++;
        else { printf("  FAIL  inode counts not sane\n"); g_fails++; }
    }

    /* PSRAM carve check — stage 2a. */
    printf("--- PSRAM pool carve ---\n");
    uint32_t szs[3] = {0,0,0};
    p_szs(szs);
    printf("  cart-trashfs = %u bytes\n  audio        = %u bytes\n"
           "  L2           = %u bytes\n  total        = %u bytes\n",
           szs[0], szs[1], szs[2], szs[0]+szs[1]+szs[2]);
    if (szs[0] == 1u*1024u*1024u) g_passes++;
    else { printf("  FAIL  cart-trashfs size\n"); g_fails++; }
    if (szs[1] == 4u*1024u*1024u) g_passes++;
    else { printf("  FAIL  audio size\n"); g_fails++; }
    if (szs[2] == 3u*1024u*1024u) g_passes++;
    else { printf("  FAIL  L2 size\n"); g_fails++; }
    if (szs[0]+szs[1]+szs[2] == 8u*1024u*1024u) g_passes++;
    else { printf("  FAIL  total size\n"); g_fails++; }

    /* Initial state checks. window[$8000] is the boot stub's first
     * instruction (auto-loaded if the smoke ROM was baked in); we
     * don't pin a specific value because the boot stub may evolve. */
    printf("\n--- post-init: status = KERNEL_RDY, frame-ready = 0 ---\n");
    check_eq_u8("status byte at $C0:7F00 == 0x80",
                p_read(0xC07F00u), 0x80);
    check_eq_u8("frame-ready at $C0:7800 == 0x00",
                p_read(0xC07800u), 0x00);

    /* Joypad mailbox read: returns don't-care, side-effect latch. */
    printf("\n--- joypad mailbox decode ---\n");
    p_read(0xC07200u);  /* P1 LO  */
    p_read(0xC07500u);  /* P2 HI  */
    /* No external check here yet (no peek-internal-state public API);
     * we just confirm the reads don't crash and return 0 don't-care. */
    check_eq_u8("P1 LO mailbox returns 0 (don't-care)",
                p_read(0xC07200u), 0x00);

    /* Strobe boot: one-shot side effect clears KERNEL_RDY. */
    printf("\n--- boot strobe side effect ---\n");
    check_eq_u8("status pre-strobe == 0x80",
                p_read(0xC07F00u), 0x80);
    (void)p_read(0xC07E00u);
    check_eq_u8("status post-strobe == 0x00",
                p_read(0xC07F00u), 0x00);

    /* The smoke ROM is auto-loaded by mgapi_init when baked in.
     * Verify the reset vector and HiROM tag show up where the SNES
     * looks for them — no dev_load_blob hop required. (The unused
     * sfc_path arg is kept for backwards compatibility.) */
    (void)sfc_path; (void)read_file; (void)p_load;
    printf("\n--- auto-loaded smoke ROM: verify cart vectors ---\n");
    {
        uint8_t lo = p_read(0x00FFFCu);
        uint8_t hi = p_read(0x00FFFDu);
        printf("  reset vector = $%02x%02x\n", hi, lo);
        if (lo == 0 && hi == 0) {
            printf("  (skipped: smoke ROM not baked in - run snes\\build.ps1 -Smoke first)\n");
        } else {
            check_eq_u8("reset vector low  byte == 0x00", lo, 0x00);
            check_eq_u8("reset vector high byte == 0x80", hi, 0x80);
            check_eq_u8("map mode $FFD5 == 0x21",
                        p_read(0x00FFD5u), 0x21);
            check_eq_u8("mirror: $C0:FFFC matches $00:FFFC",
                        p_read(0xC0FFFCu), lo);
            check_eq_u8("mirror: $C0:FFD5 matches $00:FFD5",
                        p_read(0xC0FFD5u), 0x21);
        }
    }

    /* Joypad post then verify subsequent mailbox reads don't crash. */
    printf("\n--- post pads, walk through mailbox once ---\n");
    uint16_t pads[4] = { 0xBEEF, 0xCAFE, 0x1234, 0x5678 };
    p_pads(pads);
    /* Walk every port; each must return 0 don't-care. */
    for (int port = 0; port < 8; port++) {
        uint32_t addr = 0xC07000u + ((uint32_t)port << 8);
        uint8_t got = p_read(addr);
        if (got != 0) {
            printf("  FAIL  port %d at 0x%06x returned 0x%02x (expected 0)\n",
                   port, addr, got);
            g_fails++;
        } else {
            g_passes++;
        }
    }
    printf("  mailbox walk: 8 ports, all returned 0 don't-care\n");

    /* Stage 2b1: audio pump + drain via the ring. */
    printf("\n--- audio pump fills ring; drain empties it ---\n");
    {
        /* Buffer big enough to absorb the largest pull the test asks for —
         * 8192 stereo frames = 32 KB. Stays well clear of the default
         * Win32 main-thread stack and lets us prove the short-pull path
         * without risking a stack smash. */
        static int16_t buf[8192 * 2];
        uint32_t rs[2] = {0,0};

        p_ring(rs);
        printf("  ring used pre-step = %u / cap = %u\n", rs[0], rs[1]);
        if (rs[0] == 0)  g_passes++; else { printf("  FAIL  ring not empty before pump\n"); g_fails++; }
        if (rs[1] > 0)   g_passes++; else { printf("  FAIL  ring capacity zero\n"); g_fails++; }

        /* One 60 Hz frame at 44.1 kHz = 735 frames. Pump elapsed = 16.6 ms. */
        p_step(16666666ULL);
        p_ring(rs);
        printf("  ring used post-step = %u\n", rs[0]);
        if (rs[0] >= 700 && rs[0] <= 800)  g_passes++;
        else { printf("  FAIL  ring used outside ~735 range\n"); g_fails++; }

        /* Drain 256 frames — should reduce used by 256 and write bytes. */
        memset(buf, 0xAA, sizeof buf);  /* sentinel before drain */
        uint32_t got = p_pull(buf, 256);
        printf("  pull(256) -> %u frames\n", got);
        if (got == 256)  g_passes++;
        else { printf("  FAIL  pull(256) returned %u\n", got); g_fails++; }

        /* Stage 2b1 mixer is silent (no voices triggered). Verify the
         * sentinel was overwritten — we got *some* samples, even if zero. */
        int wrote_zeros = 1;
        for (int i = 0; i < 16; i++) {
            if ((uint8_t)((const uint8_t*)buf)[i] == 0xAA) {
                wrote_zeros = 0; break;
            }
        }
        if (wrote_zeros) g_passes++;
        else { printf("  FAIL  drain didn't overwrite sentinel\n"); g_fails++; }

        p_ring(rs);
        uint32_t remaining = rs[0];
        printf("  ring used post-drain = %u\n", remaining);
        if (remaining < 600)  g_passes++;
        else { printf("  FAIL  drain didn't shrink ring\n"); g_fails++; }

        /* Pulling more than is available returns the short count. */
        uint32_t got2 = p_pull(buf, 4096);
        printf("  pull(4096, ring=%u) -> %u (short = expected)\n", remaining, got2);
        if (got2 == remaining)  g_passes++;
        else { printf("  FAIL  short-pull expected %u got %u\n", remaining, got2); g_fails++; }
    }

    /* Stage 2d: L2 allocator round-trip. */
    printf("\n--- L2 allocator on the 3 MB PSRAM slice ---\n");
    {
        uint64_t s0[5], s1[5], s2[5];
        p_l2s(s0);
        printf("  initial: used=%llu free=%llu allocs=%llu blocks=%llu max=%llu\n",
               (unsigned long long)s0[0], (unsigned long long)s0[1],
               (unsigned long long)s0[2], (unsigned long long)s0[3],
               (unsigned long long)s0[4]);
        if (s0[2] == 0) g_passes++;
        else { printf("  FAIL  initial alloc_count != 0\n"); g_fails++; }
        if (s0[1] > 2u*1024u*1024u) g_passes++;
        else { printf("  FAIL  initial free_bytes < 2 MB\n"); g_fails++; }

        /* Three distinct allocations, write distinct patterns, verify
         * they don't trample each other (overlap detection). */
        void *a = p_l2a(64 * 1024, 0);   /* 64 KB */
        void *b = p_l2a(128 * 1024, 0);  /* 128 KB */
        void *c = p_l2a(16, 0);          /* tiny */
        if (a && b && c) g_passes++;
        else { printf("  FAIL  one or more allocs returned NULL\n"); g_fails++; }
        if (a != b && a != c && b != c) g_passes++;
        else { printf("  FAIL  alloc returned duplicates\n"); g_fails++; }

        memset(a, 0xAA, 64 * 1024);
        memset(b, 0x55, 128 * 1024);
        memset(c, 0xCC, 16);
        int ok = 1;
        if (((uint8_t*)a)[0] != 0xAA) ok = 0;
        if (((uint8_t*)a)[64*1024 - 1] != 0xAA) ok = 0;
        if (((uint8_t*)b)[0] != 0x55) ok = 0;
        if (((uint8_t*)b)[128*1024 - 1] != 0x55) ok = 0;
        if (((uint8_t*)c)[0] != 0xCC) ok = 0;
        if (((uint8_t*)c)[15] != 0xCC) ok = 0;
        if (ok) g_passes++;
        else { printf("  FAIL  pattern read-back mismatch\n"); g_fails++; }

        p_l2s(s1);
        printf("  after 3 allocs: used=%llu free=%llu allocs=%llu\n",
               (unsigned long long)s1[0], (unsigned long long)s1[1],
               (unsigned long long)s1[2]);
        if (s1[2] == 3) g_passes++;
        else { printf("  FAIL  alloc count != 3 (got %llu)\n",
                      (unsigned long long)s1[2]); g_fails++; }
        if (s1[1] < s0[1]) g_passes++;
        else { printf("  FAIL  free didn't shrink\n"); g_fails++; }

        /* Free middle block; alloc a similar-sized block; verify the
         * freed space gets reused (free shrinks by less than full
         * 128 KB worth, since we just claimed back a chunk). */
        size_t free_before_realloc = s1[1];
        p_l2f(b);
        void *b2 = p_l2a(128 * 1024, 0);
        if (b2 != NULL) g_passes++;
        else { printf("  FAIL  realloc returned NULL\n"); g_fails++; }

        /* After alloc(128 KB) then free(128 KB) then alloc(128 KB),
         * net free should be ~equal to before-realloc (the freed
         * space was reused). */
        p_l2s(s2);
        printf("  after re-alloc: used=%llu free=%llu allocs=%llu\n",
               (unsigned long long)s2[0], (unsigned long long)s2[1],
               (unsigned long long)s2[2]);
        if (s2[2] == 3) g_passes++;
        else { printf("  FAIL  alloc count after realloc != 3\n"); g_fails++; }
        /* Free should be approximately the same before/after the
         * free+alloc cycle — within a header's worth. */
        long long diff = (long long)s2[1] - (long long)free_before_realloc;
        if (diff > -64 && diff < 64) g_passes++;
        else { printf("  FAIL  free bytes drifted by %lld (expected ~0)\n",
                      diff); g_fails++; }

        /* Cleanup. After freeing everything, allocator should return
         * to the initial state (modulo coalescing back into one
         * giant free block). */
        p_l2f(a); p_l2f(b2); p_l2f(c);
        uint64_t s3[5]; p_l2s(s3);
        printf("  after free-all: used=%llu free=%llu allocs=%llu blocks=%llu\n",
               (unsigned long long)s3[0], (unsigned long long)s3[1],
               (unsigned long long)s3[2], (unsigned long long)s3[3]);
        if (s3[2] == 0) g_passes++;
        else { printf("  FAIL  alloc count after free-all != 0\n"); g_fails++; }
        if (s3[3] == 1) g_passes++;
        else { printf("  FAIL  coalesce: expected 1 free block, got %llu\n",
                      (unsigned long long)s3[3]); g_fails++; }
        if (s3[1] >= s0[1]) g_passes++;
        else { printf("  FAIL  free didn't restore to initial level\n"); g_fails++; }
    }

    /* Stage 3a: VM system + shell ELF boot. */
    printf("\n--- VM system + shell ELF ---\n");
    {
        uint32_t vs[5] = {0,0,0,0,0};
        p_vm(vs);
        printf("  has_shell_elf=%u  shell_elf_bytes=%u\n", vs[0], vs[1]);
        printf("  vm_system_alive=%u  shell_loaded=%u  shell_vm_id=%u\n",
               vs[2], vs[3], vs[4]);

        if (vs[2] == 1) g_passes++;
        else { printf("  FAIL  vm_system not alive\n"); g_fails++; }

        /* If shell ELF was baked in, it should have loaded. (-NoGuest
         * build would have len=0 → no load, which we don't fail on
         * because that build path is intentional.)
         * Note: vm_id 0 is a valid assigned ID (first VM); we only
         * fail on shell_loaded == 0. */
        if (vs[0] == 1) {
            if (vs[3] == 1) g_passes++;
            else { printf("  FAIL  shell ELF present but not loaded\n"); g_fails++; }
        } else {
            printf("  (skipped shell-load check: -NoGuest build)\n");
        }

        /* Step the runtime several times. Stage 3a goal: scheduler
         * advances without crashing. The shell will quickly block
         * trying to read stdin (we haven't bound a transport yet);
         * idle is the expected end state. */
        for (int i = 0; i < 5; i++) {
            p_step(16666666ULL);   /* one 60Hz frame each */
        }
        printf("  scheduler ran 5 frames without crash\n");
        g_passes++;
    }

    /* Stage 3c: end-to-end guest L2 round-trip. The l2_test ELF
     * allocates three L2 chunks, writes/reads patterns, frees, and
     * exits 0 if all checks passed. Verifies the VM core SHARED-
     * split + SYS_L2_ALLOC/FREE + bit-29 translation end-to-end. */
    printf("\n--- L2 guest round-trip (l2_test.elf) ---\n");
    {
        int exit_code = p_l2t();
        printf("  l2_test exit code = %d\n", exit_code);
        if (exit_code == 0) {
            g_passes++;
            printf("  PASS  alloc_l2/free_l2 round-trip from a guest works\n");
        } else if (exit_code == -ENOENT) {
            printf("  (skipped: l2_test ELF not embedded — -NoGuest build?)\n");
        } else {
            g_fails++;
            const char *what =
                exit_code == 1 ? "alloc_l2 returned NULL" :
                exit_code == 2 ? "pointer outside 0xE000_0000 range" :
                exit_code == 3 ? "pattern read-back mismatch" :
                exit_code == 4 ? "freed space not reused on realloc" :
                                 "(unknown)";
            printf("  FAIL  exit %d (%s)\n", exit_code, what);
        }
    }

    /* Copro-driven menu: spawn menu.elf, let it stage one frame,
     * then peek at the cart window to confirm the bytes match what
     * the SNES kernel expects to DMA. Proves the menu guest works
     * end-to-end without needing bsnes.
     *
     * Reset the joypad mailbox first — earlier in this test we
     * posted 0xBEEF to exercise the mailbox decode, which has bit 10
     * (JOY_DN) set. If left in, menu.elf reads that on its first
     * frame as a fresh DOWN-edge and parks the arrow at row 14. */
    printf("\n--- copro menu (menu.elf) stages a frame ---\n");
    {
        uint16_t zero_pads[4] = {0,0,0,0};
        p_pads(zero_pads);

        int rc = p_men();
        printf("  stage_menu_one_frame rc = %d\n", rc);
        if (rc == 0) g_passes++;
        else if (rc == -2) {
            printf("  (skipped: menu.elf not embedded)\n");
        } else {
            printf("  FAIL  stage rc != 0\n"); g_fails++;
        }

        if (rc == 0) {
            /* CGRAM at window byte $0000 = { 0x00, 0x00, 0xFF, 0x7F } */
            check_eq_u8("menu CGRAM[0] lo = 0x00",
                        p_read(0xC00000u), 0x00);
            check_eq_u8("menu CGRAM[1] hi = 0x7F (white BGR555)",
                        p_read(0xC00003u), 0x7F);

            /* Tilemap at window byte $0004; "S" tile (id 11) sits at
             * row 8 col 10 -> entry index 8*32+10 = 266 -> byte
             * offset 0x0004 + 266*2 = 0x0218. */
            check_eq_u8("title[0] = T_S (id 11) at ($8, 10)",
                        p_read(0xC00218u), 11);
            check_eq_u8("title[1] high byte = 0x00 (palette/flip = 0)",
                        p_read(0xC00219u), 0x00);

            /* Arrow at row 12 col 9 -> entry 12*32+9 = 393 -> byte
             * offset 0x0004 + 393*2 = 0x0316. arrow_pos = 0 (fresh
             * VM) → arrow_row = ROW_OPT0 = 12. */
            check_eq_u8("arrow at ($12, 9) = T_ARR (id 16)",
                        p_read(0xC00316u), 16);

            /* DMA list slot 0 (CGRAM upload): bbus=$22, dmap=$00,
             * src=0x0000, size=4, prep=0x0000. Bytes at $7808+. */
            check_eq_u8("dma[0] bbus = $22 (CGDATA)",
                        p_read(0xC07808u), 0x22);
            check_eq_u8("dma[0] dmap = $00 (1 byte/reg)",
                        p_read(0xC07809u), 0x00);
            check_eq_u8("dma[0] src low = 0x00",
                        p_read(0xC0780Au), 0x00);
            check_eq_u8("dma[0] size low = 4",
                        p_read(0xC0780Cu), 0x04);

            /* DMA list slot 1 (CHR upload): bbus=$18, dmap=$01,
             * size = 17*32 = 544 = 0x0220, prep = 0x1000 (CHR base). */
            check_eq_u8("dma[1] bbus = $18 (VMDATAL)",
                        p_read(0xC07810u), 0x18);
            check_eq_u8("dma[1] dmap = $01 (word)",
                        p_read(0xC07811u), 0x01);
            check_eq_u8("dma[1] prep high = 0x10 (CHR base $1000)",
                        p_read(0xC07817u), 0x10);

            /* DMA list slot 2 (tilemap upload): bbus=$18, dmap=$01,
             * size = 0x0800 (2048), prep = 0x0000 (tilemap base 0). */
            check_eq_u8("dma[2] bbus = $18 (VMDATAL)",
                        p_read(0xC07818u), 0x18);

            /* Frame-ready byte was committed by the guest. */
            check_eq_u8("frame_ready committed (non-zero)",
                        p_read(0xC07800u), 0x01);
        }
    }

    /* Cart-reset marshalling: begin captures a timer; ready returns
     * false until hold_ms has elapsed, then flips to true. Window
     * vectors stay valid throughout (re-staged from the ROM). */
    /* Spawn each bundled demo for a small number of scheduler steps
     * and report the return code. This goes through the same loader
     * path the shell's `run` command uses (vm_system_load_vm), so
     * a load failure here = a load failure when the user types
     * `run /td0/demos/<name>.elf`. The "steps" count is enough for
     * the demo to reach its first mg_wait_frame; we don't need to
     * see frames committed, just confirm load + first PC steps work. */
    printf("\n--- bundled demos: install check (/td0/demos/) ---\n");
    if (p_td0) {
        static const char *DEMOS[] = { "palette", "letterbox", "sprite", "mode7" };
        for (int i = 0; i < (int)(sizeof DEMOS / sizeof DEMOS[0]); i++) {
            int sz = p_td0(DEMOS[i]);
            printf("  /td0/demos/%s.elf  size = %d\n", DEMOS[i], sz);
            if (sz > 0) g_passes++;
            else { printf("  FAIL  not installed\n"); g_fails++; }
        }
    } else {
        printf("  (skipped: mgapi_dev_td0_demo_size not exported)\n");
    }

    printf("\n--- bundled demos (load + step probe) ---\n");
    if (p_dem) {
        static const char *DEMOS[] = { "palette", "letterbox", "sprite", "mode7" };
        for (int i = 0; i < (int)(sizeof DEMOS / sizeof DEMOS[0]); i++) {
            int rc = p_dem(DEMOS[i], 5000);
            printf("  spawn '%-9s' for_steps rc = %d\n", DEMOS[i], rc);
            if (rc == 0) g_passes++;
            else if (rc == -ENOENT) {
                printf("  (skipped: %s.elf not embedded)\n", DEMOS[i]);
            } else {
                printf("  FAIL  spawn rc != 0\n"); g_fails++;
            }
        }
    } else {
        printf("  (skipped: mgapi_dev_spawn_demo_for_steps not exported)\n");
    }

    printf("\n--- bundled demos (slurp from /td0/ + spawn) ---\n");
    if (p_dtf) {
        static const char *DEMOS[] = { "palette", "letterbox", "sprite", "mode7" };
        for (int i = 0; i < (int)(sizeof DEMOS / sizeof DEMOS[0]); i++) {
            int rc = p_dtf(DEMOS[i]);
            printf("  trashfs+spawn '%-9s' rc = %d\n", DEMOS[i], rc);
            if (rc == 0) g_passes++;
            else { printf("  FAIL  trashfs+spawn rc != 0\n"); g_fails++; }
        }
    } else {
        printf("  (skipped: mgapi_dev_spawn_demo_via_trashfs not exported)\n");
    }

    printf("\n--- cart reset: timer-based hold ---\n");
    {
        /* Dirty the window first so we can prove reset re-stages it. */
        uint8_t junk[16];
        memset(junk, 0xEE, sizeof junk);
        p_load(0x00FFC0u, junk, sizeof junk);
        check_eq_u8("pre-reset: dirty byte at $FFC0", p_read(0x00FFC0u), 0xEE);

        DWORD t_begin = GetTickCount();
        p_rsb();
        /* Immediately after begin: ready should be false (hold_ms=20). */
        int rdy_immediate = p_rsr();
        if (rdy_immediate == 0) g_passes++;
        else { printf("  FAIL  ready==true immediately after begin\n"); g_fails++; }

        /* The vectors should be restored from the ROM. */
        check_eq_u8("post-reset: $FFC0 restored from ROM",
                    p_read(0x00FFC0u), 'M'); /* "MICROGARBAGE SMOKE"... */
        check_eq_u8("post-reset: reset vector low  = 0x00",
                    p_read(0x00FFFCu), 0x00);
        check_eq_u8("post-reset: reset vector high = 0x80",
                    p_read(0x00FFFDu), 0x80);

        /* Poll until ready. Should take ~hold_ms wall-clock. */
        int polled = 0;
        while (!p_rsr()) {
            Sleep(1);
            if (++polled > 1000) break;
        }
        DWORD t_done = GetTickCount();
        DWORD elapsed_ms = t_done - t_begin;
        printf("  ready after %lu ms (hold_ms=20)\n", (unsigned long)elapsed_ms);
        if (elapsed_ms >= 18 && elapsed_ms < 200) g_passes++;
        else { printf("  FAIL  elapsed=%lu outside expected 18..200\n",
                      (unsigned long)elapsed_ms); g_fails++; }

        p_rse();
        if (p_rsr() == 1) g_passes++;
        else { printf("  FAIL  ready==0 after end\n"); g_fails++; }
    }

    printf("\n%s: %d pass / %d fail\n",
           g_fails == 0 ? "RESULT" : "RESULT", g_passes, g_fails);

    /* --tcp <port> mode: stay alive forever stepping the runtime so
     * a PuTTY raw-mode client can shell in and 'run' a demo ELF.
     * One mgapi_step per ~16.6 ms keeps the scheduler advancing at
     * roughly NTSC vblank cadence; the audio ring stays drained by
     * the test loop above. Press Ctrl-C to kill. */
    if (tcp_port != 0) {
        printf("\nlistening on TCP %u — PuTTY raw-connect to drive the shell\n",
               (unsigned)tcp_port);
        printf("Ctrl-C to exit. /td0/demos/{palette,letterbox,sprite,mode7}.elf available.\n");
        fflush(stdout);
        SetConsoleCtrlHandler(ctrlc_handler, TRUE);
        signal(SIGINT,  sigint_handler);
        signal(SIGTERM, sigint_handler);
        /* Last-resort stdin watcher — fires on Ctrl-C byte (0x03) or
         * 'q' from MSYS/Git-Bash terminals where the other two paths
         * don't reach us. Detached; the process exits when the loop
         * sees g_ctrlc_seen, so we don't bother joining. */
        HANDLE wt = CreateThread(NULL, 0, stdin_watcher, NULL, 0, NULL);
        if (wt) CloseHandle(wt);
        printf("(also: press q + Enter to quit)\n");
        fflush(stdout);
        const uint64_t step_ns = 16666667ull;   /* ~60 Hz */
        while (!g_ctrlc_seen) {
            p_step(step_ns);
            /* 16 ms sleep keeps host CPU sane. The DLL's TCP listener
             * runs on its own thread so its progress is independent
             * of how fast we tick. */
            Sleep(16);
        }
        printf("\nCtrl-C — shutting down...\n");
        fflush(stdout);
    }

    p_shut();
    FreeLibrary(m);

    return g_fails == 0 ? 0 : 1;
}
