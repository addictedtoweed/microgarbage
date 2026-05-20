/* 05_shell/host.c — run the file-system shell guest.
 *
 * Sets up the full stack:
 *   - 64 KB trashdrive (RAM block device)
 *   - FatFs filesystem mounted at "0:/" (format-on-start)
 *   - VmSystem with stdio bridge and file syscalls installed
 *   - shell.elf loaded as the sole VM
 *
 * Then runs the scheduler until the shell calls SYS_EXIT (or
 * Ctrl-C from the user).
 *
 * Stdio routing:
 *   default        process stdin/stdout/stderr (current behavior)
 *   --pipe=<name>  bidirectional Windows named pipe; PuTTY (or
 *                  another client) connects to it as a "serial"
 *                  line. Decouples VM execution from the local
 *                  terminal's scheduling/rendering and is also a
 *                  realistic stand-in for a UART on real hardware.
 *
 * Build dependencies (beyond the standard -Iinclude):
 *   -Ithird_party/fatfs/source -Ithird_party/fatfs -DHAVE_FATFS
 *
 * Without those flags this example will fail to build because
 * FatFs symbols (f_mount, f_mkfs, etc.) won't resolve. See the
 * build.sh in this directory for the full link line.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE                /* needed for fopencookie on Cygwin */

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_fs.h"
#include "storage/trashdrive.h"
#include "storage/trashdrive_fatfs.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if defined(__CYGWIN__) || defined(_WIN32)
#  define PIPE_MODE_SUPPORTED 1
#  include <windows.h>
#  include <fcntl.h>       /* O_RDWR */
#  include <unistd.h>      /* close, ssize_t */
#  if !defined(__CYGWIN__)
#    include <io.h>           /* _open_osfhandle on MSVC/MinGW */
#  endif
#endif

/* ---------------------------------------------------------------
 * Backing storage
 *
 * The trashdrive pool needs to be large enough for FatFs R0.16 to
 * lay out a valid FAT volume. Empirically the minimum on R0.16
 * with our config (FM_FAT, n_fat=1) is around 96 KB; below that
 * f_mkfs returns FR_MKFS_ABORTED. We use 128 KB to leave headroom
 * for files plus FatFs's own bookkeeping.
 *
 * If you change this, keep it a multiple of TRASH_SECTOR_SIZE (512).
 * --------------------------------------------------------------- */
#define POOL_BYTES   (128 * 1024)
#define SHARED_BYTES (64 * 1024)
#define LOCAL_BYTES  (96 * 1024)

static uint8_t g_pool[POOL_BYTES];
static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

static TrashDrive g_drive;
static FATFS g_fs;

/* ---------------------------------------------------------------
 * Tick source: milliseconds since first call.
 *
 * The scheduler calls this on each step to refresh global_tick.
 * Guests see tick units of one millisecond, and SYS_TICK_HZ
 * returns 1000. The tick wraps at 2^32 ms ≈ 49.7 days of
 * continuous runtime.
 *
 * We anchor at the first call so global_tick starts at 0 (or
 * close to it) — easier to reason about than raw monotonic time
 * which can be a giant number. CLOCK_MONOTONIC is immune to
 * wall-clock adjustments (NTP, manual set).
 * --------------------------------------------------------------- */
static struct timespec g_t0;
static int             g_t0_set = 0;

static uint32_t monotonic_ms_ticks(void *userdata) {
    (void)userdata;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!g_t0_set) {
        g_t0 = t;
        g_t0_set = 1;
    }
    /* Difference in milliseconds. The cast truncates to uint32,
     * which is the intended wraparound behavior. */
    uint64_t ms =
        ((uint64_t)(t.tv_sec  - g_t0.tv_sec )) * 1000ULL +
        ((uint64_t)(t.tv_nsec - g_t0.tv_nsec)) / 1000000ULL;
    return (uint32_t)ms;
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "host: cannot open '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -1; }
    *out_buf = buf; *out_size = (size_t)sz;
    return 0;
}

#ifdef PIPE_MODE_SUPPORTED
/* ---------------------------------------------------------------
 *  Named-pipe transport (Windows-only)
 *
 *  Creates a bidirectional Windows named pipe and waits for a
 *  single client to connect. The pipe is byte-oriented (not
 *  message-oriented) and configured to behave as much like a tty
 *  as we can — that means PIPE_READMODE_BYTE on our side and the
 *  client (PuTTY) just reads bytes as they arrive.
 *
 *  Returns a FILE* that can be used for both reading and writing.
 *  The same FILE* is suitable for stdin AND stdout/stderr because
 *  Windows named pipes are full-duplex; the VM's stdio bridge
 *  will end up wrapping the same underlying HANDLE three times,
 *  which is exactly what we want — bytes the guest "writes to
 *  stderr" arrive at the PuTTY end interleaved with stdout bytes,
 *  same as a real serial line.
 *
 *  On Cygwin we don't use cygwin_attach_handle_to_fd + fdopen —
 *  that combination fails with EBADF on some Cygwin versions
 *  because the fd produced by cygwin_attach_handle_to_fd lacks
 *  some metadata fdopen expects. Instead we use fopencookie
 *  (a GNU extension Cygwin supports) and call ReadFile/WriteFile
 *  directly in the callbacks. This sidesteps Cygwin's fd table
 *  entirely.
 *
 *  Native MinGW / MSVC builds use the simpler _open_osfhandle +
 *  fdopen path since those CRTs handle it correctly.
 *
 *  Why this lives in host.c instead of vm_host_stdio.c:
 *    - It's host-application-policy (which transport to use) rather
 *      than VM-bridge functionality (how the guest sees stdio).
 *    - It's Windows-specific; the bridge is portable.
 *    - Future hosts (TCP socket, /dev/ttyUSBn, etc.) plug in here
 *      using the same pattern.
 * --------------------------------------------------------------- */

static HANDLE g_pipe_handle = INVALID_HANDLE_VALUE;

#if defined(__CYGWIN__)
/* fopencookie callbacks — talk directly to the HANDLE. The cookie
 * is the HANDLE itself, cast to void*. */

static ssize_t pipe_cookie_read(void *cookie, char *buf, size_t size) {
    HANDLE h = (HANDLE)cookie;

    /* Non-blocking read via PeekNamedPipe.
     *
     * We CAN'T put the pipe into PIPE_NOWAIT mode, because that
     * affects the client side too — PuTTY's ReadFile would then
     * also return immediately when no data is queued, which
     * PuTTY interprets as EOF, causing it to disconnect. So we
     * keep the pipe in blocking mode and use PeekNamedPipe to
     * check the queue before each ReadFile.
     *
     * PeekNamedPipe returns the number of bytes currently in the
     * pipe's read buffer without removing them. If that's zero,
     * we report read-returned-0 (no data) without calling
     * ReadFile (which would block). If it's nonzero, we
     * ReadFile up to that many bytes — guaranteed not to block. */
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            return 0;          /* PuTTY closed → EOF */
        }
        errno = EIO;
        return -1;
    }
    if (avail == 0) {
        return 0;              /* No data available right now */
    }

    DWORD want = (DWORD)size;
    if (want > avail) want = avail;
    DWORD got = 0;
    if (!ReadFile(h, buf, want, &got, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            return 0;
        }
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

static ssize_t pipe_cookie_write(void *cookie, const char *buf, size_t size) {
    HANDLE h = (HANDLE)cookie;
    DWORD wrote = 0;
    if (!WriteFile(h, buf, (DWORD)size, &wrote, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            errno = EPIPE;
            return -1;
        }
        errno = EIO;
        return -1;
    }
    return (ssize_t)wrote;
}

static int pipe_cookie_close(void *cookie) {
    HANDLE h = (HANDLE)cookie;
    CloseHandle(h);
    return 0;
}
#endif  /* __CYGWIN__ */

static FILE *open_named_pipe_for_stdio(const char *name) {
    /* Pipe naming: callers can pass either a fully-qualified
     * "\\\\.\\pipe\\foo" or a short "foo". Translate short forms
     * to the full prefix to make the CLI friendlier. */
    char full[256];
    if (strncmp(name, "\\\\.\\pipe\\", 9) == 0) {
        snprintf(full, sizeof(full), "%s", name);
    } else {
        snprintf(full, sizeof(full), "\\\\.\\pipe\\%s", name);
    }

    /* PIPE_ACCESS_DUPLEX     — bidirectional
     * PIPE_TYPE_BYTE         — stream-oriented, not message-oriented
     * PIPE_WAIT initially    — needed for the blocking ConnectNamedPipe
     *                          below; switched to NOWAIT after connect
     * 1 instance, 4 KB buffers, default timeout. */
    g_pipe_handle = CreateNamedPipeA(
        full,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                     /* max instances */
        4096, 4096,            /* out, in buffer sizes */
        0,                     /* default timeout */
        NULL);                 /* default security */
    if (g_pipe_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "host: CreateNamedPipe('%s') failed (error %lu)\n",
                full, (unsigned long)GetLastError());
        return NULL;
    }

    fprintf(stderr, "host: waiting for client on %s ...\n", full);
    fprintf(stderr, "host: in PuTTY: Session type=Serial, "
                    "Serial line=%s, Speed=any\n", full);
    fflush(stderr);

    /* Blocks until a client connects. ERROR_PIPE_CONNECTED means
     * the client connected between CreateNamedPipe and here,
     * which is fine. */
    if (!ConnectNamedPipe(g_pipe_handle, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            fprintf(stderr, "host: ConnectNamedPipe failed (error %lu)\n",
                    (unsigned long)err);
            CloseHandle(g_pipe_handle);
            g_pipe_handle = INVALID_HANDLE_VALUE;
            return NULL;
        }
    }
    fprintf(stderr, "host: client connected.\n");
    fflush(stderr);

    /* Note: pipe stays in blocking mode. We don't use PIPE_NOWAIT
     * because that affects the CLIENT side too — PuTTY's ReadFile
     * would return 0 bytes immediately when nothing's queued,
     * which PuTTY interprets as EOF and disconnects. We do non-
     * blocking reads on our side via PeekNamedPipe inside the
     * read cookie. */

    /* Build a FILE* over the HANDLE.
     *
     * On Cygwin we use fopencookie with our own ReadFile/WriteFile
     * callbacks — sidesteps the Cygwin fd-table issues that make
     * cygwin_attach_handle_to_fd + fdopen fail with EBADF on some
     * versions.
     *
     * On native MinGW/MSVC we use the standard _open_osfhandle +
     * fdopen path, which works correctly there. */
#if defined(__CYGWIN__)
    cookie_io_functions_t cb = {
        .read  = pipe_cookie_read,
        .write = pipe_cookie_write,
        .seek  = NULL,       /* not seekable — pipes never are */
        .close = pipe_cookie_close,
    };
    FILE *f = fopencookie((void *)g_pipe_handle, "r+", cb);
    if (!f) {
        fprintf(stderr, "host: fopencookie failed: %s\n", strerror(errno));
        CloseHandle(g_pipe_handle);
        g_pipe_handle = INVALID_HANDLE_VALUE;
        return NULL;
    }
#else
    int fd = _open_osfhandle((intptr_t)g_pipe_handle, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "host: _open_osfhandle failed: %s\n",
                strerror(errno));
        CloseHandle(g_pipe_handle);
        g_pipe_handle = INVALID_HANDLE_VALUE;
        return NULL;
    }
    FILE *f = fdopen(fd, "rb+");
    if (!f) {
        fprintf(stderr, "host: fdopen failed: %s\n", strerror(errno));
        close(fd);
        g_pipe_handle = INVALID_HANDLE_VALUE;
        return NULL;
    }
#endif

    /* No buffering — we want every byte to flow immediately, the
     * same way it does on a tty. The VM's SYS_FFLUSH calls won't
     * hurt but with _IONBF they're effectively no-ops. */
    setvbuf(f, NULL, _IONBF, 0);
    return f;
}
#endif  /* PIPE_MODE_SUPPORTED */

int main(int argc, char **argv) {
    /* ----- Parse args -----
     *
     * Usage: host [options] [shell.elf]
     *
     * Options:
     *   --host-fs=<path>    Mount <path> as /host inside the shell.
     *                       The guest can then read/run files via
     *                       /host/<name>. Default: ./host_files
     *   --host-fs-rw        Make the /host mount writable. Default
     *                       is read-only for safety.
     *   --no-host-fs        Disable the /host mount entirely.
     *   --pipe=<name>       (Windows/Cygwin) Route stdio through a
     *                       named pipe; PuTTY connects to it as a
     *                       Serial session. Name can be a bare
     *                       identifier ('microgarbage') or a full
     *                       \\.\\pipe\\<name> path.
     *
     * Positional: the path to shell.elf. Defaults to build/shell.elf.
     */
    const char *elf_path     = NULL;
    const char *host_fs_root = "host_files";   /* default — created if missing */
    bool host_fs_writable    = false;
    bool host_fs_disabled    = false;
    const char *pipe_name    = NULL;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--host-fs=", 10) == 0) {
            host_fs_root = argv[i] + 10;
        } else if (strcmp(argv[i], "--host-fs-rw") == 0) {
            host_fs_writable = true;
        } else if (strcmp(argv[i], "--no-host-fs") == 0) {
            host_fs_disabled = true;
        } else if (strncmp(argv[i], "--pipe=", 7) == 0) {
            pipe_name = argv[i] + 7;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "host: unknown option '%s'\n", argv[i]);
            fprintf(stderr, "  --host-fs=<path>   mount path as /host (default: ./host_files)\n");
            fprintf(stderr, "  --host-fs-rw       allow writes to /host (default: read-only)\n");
            fprintf(stderr, "  --no-host-fs       disable /host mount\n");
            fprintf(stderr, "  --pipe=<name>      route stdio through a named pipe (Windows)\n");
            return 1;
        } else if (!elf_path) {
            elf_path = argv[i];
        } else {
            fprintf(stderr, "host: extra positional argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!elf_path) elf_path = "build/shell.elf";

#ifndef PIPE_MODE_SUPPORTED
    if (pipe_name) {
        fprintf(stderr, "host: --pipe is Windows-only "
                        "(this build targets a non-Windows platform).\n");
        return 1;
    }
#endif

    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    /* 1. Initialize the block device. */
    if (trash_init(&g_drive, g_pool, sizeof(g_pool)) != TRASH_OK) {
        fprintf(stderr, "host: trash_init failed\n");
        return 1;
    }

    /* 2. Register with FatFs as drive 0. */
    if (!trash_fatfs_register(0, &g_drive)) {
        fprintf(stderr, "host: trash_fatfs_register failed\n");
        return 1;
    }

    /* 3. Format the volume (always — trashdrive is RAM so we
     * start fresh each run). For persistence between runs you'd
     * skip f_mkfs and just f_mount; FatFs auto-detects a
     * pre-formatted volume. */
    BYTE work[FF_MAX_SS];
    MKFS_PARM opt = {0};
    opt.fmt = FM_FAT;
    opt.n_fat = 1;
    FRESULT fr = f_mkfs("0:", &opt, work, sizeof(work));
    if (fr != FR_OK) {
        fprintf(stderr, "host: f_mkfs failed: %d\n", fr);
        return 1;
    }

    /* 4. Mount the volume. */
    fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) {
        fprintf(stderr, "host: f_mount failed: %d\n", fr);
        return 1;
    }

    /* Pre-create a few items in the volume so `ls` has something
     * to show on first launch. Pure convenience — remove if you
     * want a truly empty start. */
    f_mkdir("0:/home");
    f_mkdir("0:/tmp");
    {
        FIL f;
        UINT bw;
        if (f_open(&f, "0:/readme.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            const char *msg =
                "Welcome to the VM shell.\n"
                "Try: ls, cd /home, mkdir foo, touch bar.txt, cat readme.txt\n";
            f_write(&f, msg, (UINT)strlen(msg), &bw);
            f_close(&f);
        }
    }

    /* 5. Load the guest ELF. */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) {
        return 1;
    }

    /* 6. Build the VmSystem and install both bridges. */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,

        /* Real-time tick source: 1 ms granularity from
         * CLOCK_MONOTONIC. Guests can use SYS_SLEEP_TICKS and
         * SYS_SLEEP_UNTIL to pace themselves at human timescales
         * (animations, polling, periodic loops). */
        .tick_source         = monotonic_ms_ticks,
        .ticks_per_second    = 1000,
    };
    if (!vm_system_init(&sys, &cfg)) {
        fprintf(stderr, "host: vm_system_init failed\n");
        return 1;
    }

    /* Stdio install: either default (process stdin/stdout/stderr)
     * or routed through a named pipe. The pipe call BLOCKS until
     * a client connects, so the user sees the "waiting" message
     * first and then the shell banner once their PuTTY is attached. */
    if (pipe_name) {
#ifdef PIPE_MODE_SUPPORTED
        FILE *pipe_io = open_named_pipe_for_stdio(pipe_name);
        if (!pipe_io) {
            return 1;
        }
        VmHostStdioConfig sio = {
            .stdin_src   = pipe_io,
            .stdout_dest = pipe_io,
            .stderr_dest = pipe_io,
            /* No raw_mode: the pipe is already byte-at-a-time
             * and isn't a tty so the termios calls would no-op
             * anyway. The guest's SYS_TTY_SET_RAW will harmlessly
             * fail and the shell falls back to its non-raw path. */
            .raw_mode    = false,
        };
        if (!vm_host_install_stdio_ex(&sys, &sio)) {
            fprintf(stderr, "host: vm_host_install_stdio_ex failed\n");
            return 1;
        }
#else
        /* unreachable — we checked above */
        return 1;
#endif
    } else {
        if (!vm_host_install_stdio(&sys)) {
            fprintf(stderr, "host: vm_host_install_stdio failed\n");
            return 1;
        }
    }
    if (!vm_host_install_fs(&sys)) {
        fprintf(stderr, "host: vm_host_install_fs failed\n");
        return 1;
    }

    /* 6b. Configure the /host mount. By default the shell can
     * read files under ./host_files/ as /host/<name>. Lets the
     * user drop ELFs there and run them via "run /host/foo.elf"
     * inside the shell.
     *
     * If the path doesn't exist, try to create it. If we can't
     * (permissions, parent missing), warn but don't fail — the
     * shell still works without /host. */
    if (!host_fs_disabled) {
        struct stat st;
        if (stat(host_fs_root, &st) != 0) {
            /* Try to create. POSIX mkdir; succeeds in Cygwin too. */
            if (mkdir(host_fs_root, 0755) != 0) {
                fprintf(stderr, "host: warning — could not create '%s' for /host mount: %s\n",
                        host_fs_root, strerror(errno));
                fprintf(stderr, "host: /host will be disabled\n");
                host_fs_disabled = true;
            }
        }
        if (!host_fs_disabled) {
            if (!vm_host_set_host_fs_root(host_fs_root, host_fs_writable)) {
                fprintf(stderr, "host: warning — vm_host_set_host_fs_root('%s') failed\n",
                        host_fs_root);
                fprintf(stderr, "host: /host will be disabled\n");
            } else {
                fprintf(stderr, "host: /host mounted from '%s' (%s)\n",
                        host_fs_root,
                        host_fs_writable ? "read/write" : "read-only");
            }
        }
    }

    /* 7. Load the shell. */
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 16 * 1024,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d)\n", lr.code);
        return 1;
    }

    /* 8. Run. The shell never exits on its own unless the user
     * types 'exit' (or the host terminates). We use the stepping
     * loop rather than vm_system_run so SIGINT can break us out
     * cleanly. */
    for (;;) {
        if (g_stop) {
            fprintf(stderr, "\nhost: SIGINT received, stopping.\n");
            break;
        }
        VmSchedStepResult r = vm_system_step(&sys);
        if (r == VM_SCHED_ALL_HALTED) {
            break;
        }
        /* VM_SCHED_IDLE means all VMs are blocked. The shell
         * blocks on SYS_READ when the user isn't typing — but
         * our SYS_READ is non-blocking (returns 0), so the shell
         * actually loops with SYS_YIELDs. We won't see IDLE in
         * practice. */
    }

    vm_system_destroy(&sys);
    f_mount(NULL, "0:", 0);
    free(elf);
    return 0;
}
