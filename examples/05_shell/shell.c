/* shell.c — interactive file-system shell for the VM.
 *
 * Connects to host stdin/stdout via SYS_READ/SYS_WRITE and to a
 * host-mounted FatFs volume via the file syscalls in vm_host_fs.
 * Provides ls, cd, pwd, mkdir, rmdir, rm, cat, touch, write, help,
 * exit.
 *
 * Why this exists: demonstrates the file-syscall surface end to
 * end. A guest running this image can browse, create, edit, and
 * delete files in a RAM-backed FAT volume controlled by the host.
 *
 * Design choices:
 *
 * - The VM has no per-process current directory (FatFs is built
 *   with FF_FS_RPATH=0). We track CWD entirely in the guest:
 *   a static string starts at "/" and gets updated by `cd`.
 *   Relative paths from the user get prefixed with cwd before
 *   we hand them to openat/mkdirat/etc.
 *
 * - Cooked-mode stdin (no host --raw): the terminal handles
 *   line editing and delivers a whole line at Enter. We just
 *   read until '\n'. Ctrl-C in cooked mode kills the host
 *   process — which is fine for a demo.
 *
 * - No libc. All string handling is inline. Numbers and paths
 *   are kept small to fit in stack-allocated buffers.
 *
 * Public domain (CC0). No warranty.
 */

/* ============================================================
 *  Syscall numbers — keep in sync with include/vm/vm_ecall.h
 * ============================================================ */

#define SYS_MKDIRAT    34
#define SYS_UNLINKAT   35
#define SYS_OPENAT     56
#define SYS_CLOSE      57
#define SYS_LSEEK      62
#define SYS_READ       63
#define SYS_WRITE      64
#define SYS_EXIT       93
#define SYS_READDIR   120
#define SYS_YIELD    1040

#define AT_FDCWD      (-100)

#define O_RDONLY       0x000
#define O_WRONLY       0x001
#define O_RDWR         0x002
#define O_CREAT        0x040
#define O_EXCL         0x080
#define O_TRUNC        0x200
#define O_APPEND       0x400
#define O_DIRECTORY 0x10000

#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

#define AT_REMOVEDIR  0x200

#define DT_REG    0
#define DT_DIR    1

/* Errno values negated by the host on error returns. */
#define E_NOENT      2
#define E_BADF       9
#define E_EXIST     17
#define E_NOTDIR    20
#define E_ISDIR     21
#define E_INVAL     22
#define E_MFILE     24
#define E_NOSPC     28
#define E_NAMETOOLONG 36

/* Dirent layout — matches VmDirent in include/vm/vm_host_fs.h. */
typedef struct {
    unsigned type;
    unsigned size;
    char     name[64];
} Dirent;

/* ============================================================
 *  Syscall wrappers
 * ============================================================ */

static inline int sys_read(int fd, void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_READ;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_write(int fd, const void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_WRITE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_openat(int dirfd, const char *path,
                              unsigned flags, unsigned mode) {
    register int      a0 asm("a0") = dirfd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)path;
    register unsigned a2 asm("a2") = flags;
    register unsigned a3 asm("a3") = mode;
    register int      a7 asm("a7") = SYS_OPENAT;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static inline int sys_close(int fd) {
    register int a0 asm("a0") = fd;
    register int a7 asm("a7") = SYS_CLOSE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_lseek(int fd, int off, int whence) {
    register int a0 asm("a0") = fd;
    register int a1 asm("a1") = off;
    register int a2 asm("a2") = whence;
    register int a7 asm("a7") = SYS_LSEEK;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_mkdirat(int dirfd, const char *path, unsigned mode) {
    register int      a0 asm("a0") = dirfd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)path;
    register unsigned a2 asm("a2") = mode;
    register int      a7 asm("a7") = SYS_MKDIRAT;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_unlinkat(int dirfd, const char *path, unsigned flags) {
    register int      a0 asm("a0") = dirfd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)path;
    register unsigned a2 asm("a2") = flags;
    register int      a7 asm("a7") = SYS_UNLINKAT;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_readdir(int fd, Dirent *out) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)out;
    register int      a7 asm("a7") = SYS_READDIR;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline void sys_yield(void) {
    register int a7 asm("a7") = SYS_YIELD;
    asm volatile ("ecall" : : "r"(a7) : "memory");
}

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* ============================================================
 *  String helpers
 * ============================================================ */

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void scpy(char *dst, const char *src, unsigned cap) {
    unsigned i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void puts_(const char *s) {
    sys_write(1, s, slen(s));
}

static void putln(const char *s) {
    puts_(s);
    puts_("\n");
}

/* Format an unsigned integer at end of buffer; return pointer to
 * the first digit. */
static char *fmt_u32(unsigned v, char *buf_end) {
    *--buf_end = '\0';
    if (v == 0) { *--buf_end = '0'; return buf_end; }
    while (v) { *--buf_end = (char)('0' + v % 10); v /= 10; }
    return buf_end;
}

/* ============================================================
 *  Path resolution
 *
 *  Maintains a static cwd string. Resolves a user-given path to
 *  an absolute path (in `out`). Returns 0 on success, -1 on
 *  overflow.
 *
 *  Rules:
 *    "/x"      -> "/x"            (already absolute)
 *    "x"       -> "<cwd>/x"       (relative)
 *    "x/y"     -> "<cwd>/x/y"     (relative, multi-component)
 *    ""        -> "<cwd>"         (refers to cwd itself)
 *    "."       -> "<cwd>"
 *    ".."      -> parent of cwd
 *    "/"       -> "/"
 *
 *  We don't currently support arbitrary "../foo/.." patterns
 *  mid-path — only a leading ".." resolves to cwd's parent.
 *  Adding full normalization is a small refactor away but
 *  unnecessary for a demo shell.
 * ============================================================ */

#define CWD_CAP   192
#define PATH_CAP  256

static char g_cwd[CWD_CAP] = "/";

/* Find the last '/' in `path` and return its index, or -1 if
 * none. (Used to compute the parent directory for cd ..) */
static int last_slash(const char *path) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    return last;
}

static int join_paths(const char *base, const char *rest, char *out) {
    unsigned bl = slen(base);
    /* Note whether base already ends with '/' (only legitimate for
     * base == "/" — every other path is stored without trailing
     * slash). If it does, we won't add our own separator. */
    int base_ends_slash = (bl > 0 && base[bl - 1] == '/');

    /* Strip a trailing '/' from base unless base is "/" (we still
     * need to KEEP that one slash in the output). */
    if (bl > 1 && base[bl - 1] == '/') {
        bl--;
        base_ends_slash = 0;   /* we just stripped it */
    }

    unsigned rl = slen(rest);
    /* Skip leading '/' in rest (base already supplies the separator). */
    unsigned ri = 0;
    if (rl > 0 && rest[0] == '/') ri = 1;

    /* Total bytes: bl + (1 separator unless base already ends in /) +
     * (rl - ri) bytes from rest + 1 for null. */
    unsigned sep = base_ends_slash ? 0 : 1;
    unsigned total = bl + sep + (rl - ri) + 1;
    if (total > PATH_CAP) return -1;

    unsigned o = 0;
    for (unsigned i = 0; i < bl; i++) out[o++] = base[i];
    if (rl > ri) {
        if (!base_ends_slash) out[o++] = '/';
        for (unsigned i = ri; i < rl; i++) out[o++] = rest[i];
    } else if (bl == 0) {
        out[o++] = '/';
    }
    out[o] = '\0';
    return 0;
}

static int resolve_path(const char *in, char *out) {
    if (in[0] == '\0' || (in[0] == '.' && in[1] == '\0')) {
        scpy(out, g_cwd, PATH_CAP);
        return 0;
    }
    if (in[0] == '.' && in[1] == '.' && (in[2] == '\0' || in[2] == '/')) {
        /* Parent of cwd. */
        int s = last_slash(g_cwd);
        char parent[CWD_CAP];
        if (s <= 0) {
            scpy(parent, "/", CWD_CAP);
        } else {
            for (int i = 0; i < s; i++) parent[i] = g_cwd[i];
            parent[s] = '\0';
        }
        if (in[2] == '\0') {
            scpy(out, parent, PATH_CAP);
            return 0;
        }
        return join_paths(parent, in + 3, out);   /* skip "../" */
    }
    if (in[0] == '/') {
        scpy(out, in, PATH_CAP);
        return 0;
    }
    return join_paths(g_cwd, in, out);
}

/* ============================================================
 *  Error reporting
 * ============================================================ */

static const char *errno_name(int err) {
    /* err is positive here (we negate the syscall return). */
    switch (err) {
        case E_NOENT:       return "no such file or directory";
        case E_BADF:        return "bad file descriptor";
        case E_EXIST:       return "file exists";
        case E_NOTDIR:      return "not a directory";
        case E_ISDIR:       return "is a directory";
        case E_INVAL:       return "invalid argument";
        case E_MFILE:       return "too many open files";
        case E_NOSPC:       return "no space left on device";
        case E_NAMETOOLONG: return "name too long";
        default:            return "unknown error";
    }
}

static void perror_(const char *prefix, int err) {
    if (err < 0) err = -err;
    puts_(prefix);
    puts_(": ");
    puts_(errno_name(err));
    puts_("\n");
}

/* ============================================================
 *  Input
 *
 *  Read one line from host stdin (terminated by '\n'). Strips
 *  trailing '\n' and '\r'. Returns the length, or -1 if EOF/error.
 *
 *  Cooked-mode terminal: the host's terminal has line discipline
 *  enabled (no `--raw`), so Enter delivers a complete line. We
 *  just need to read until we see '\n'.
 *
 *  SYS_READ is non-blocking (returns 0 if no bytes ready), so we
 *  sys_yield between attempts to be a good scheduler citizen.
 * ============================================================ */

#define LINE_CAP  256

static int readline(char *buf, unsigned cap) {
    unsigned pos = 0;
    for (;;) {
        if (pos >= cap - 1) {
            /* Line too long; truncate and break. */
            buf[cap - 1] = '\0';
            return (int)pos;
        }
        int r = sys_read(0, buf + pos, 1);
        if (r < 0) {
            buf[pos] = '\0';
            return -1;
        }
        if (r == 0) {
            sys_yield();
            continue;
        }
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            /* Strip trailing CR if present (some terminals send CRLF). */
            if (pos > 0 && buf[pos - 1] == '\r') {
                buf[pos - 1] = '\0';
                pos--;
            }
            return (int)pos;
        }
        pos++;
    }
}

/* Split a line into argv-style tokens by whitespace. Modifies
 * the line in place (insertion of '\0' at separators). Returns
 * the token count; argv[i] points into `line`. */
static int tokenize(char *line, char **argv, int max_argv) {
    int n = 0;
    char *p = line;
    while (*p && n < max_argv) {
        /* Skip whitespace. */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* ============================================================
 *  Commands
 * ============================================================ */

static void cmd_help(void) {
    putln("commands:");
    putln("  ls [path]            list directory");
    putln("  cd <path>            change directory");
    putln("  pwd                  print working directory");
    putln("  mkdir <path>         make directory");
    putln("  rmdir <path>         remove empty directory");
    putln("  rm <path>            remove file");
    putln("  touch <path>         create empty file");
    putln("  cat <path>           print file contents");
    putln("  write <path> <text>  write text to file (truncating)");
    putln("  help                 this message");
    putln("  exit                 leave shell");
}

static void cmd_pwd(void) {
    putln(g_cwd);
}

static void cmd_ls(int argc, char **argv) {
    char path[PATH_CAP];
    const char *target = (argc >= 2) ? argv[1] : ".";
    if (resolve_path(target, path) != 0) {
        putln("ls: path too long");
        return;
    }
    int fd = sys_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { perror_("ls", fd); return; }

    Dirent de;
    int count = 0;
    for (;;) {
        int r = sys_readdir(fd, &de);
        if (r == 1) break;            /* end of directory */
        if (r < 0) { perror_("ls", r); break; }
        if (de.type == DT_DIR) {
            puts_("  ");
            puts_(de.name);
            puts_("/\n");
        } else {
            char num[16];
            char *ns = fmt_u32(de.size, num + sizeof(num));
            puts_("  ");
            /* Print name padded to 16 chars, then size. */
            unsigned nl = slen(de.name);
            puts_(de.name);
            for (unsigned i = nl; i < 16; i++) puts_(" ");
            puts_(ns);
            puts_("\n");
        }
        count++;
    }
    sys_close(fd);
    if (count == 0) putln("  (empty)");
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) { putln("cd: missing path"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("cd: path too long");
        return;
    }
    /* Verify it's a directory by opening it. */
    int fd = sys_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) { perror_("cd", fd); return; }
    sys_close(fd);
    /* Update cwd. */
    scpy(g_cwd, path, CWD_CAP);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { putln("mkdir: missing path"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("mkdir: path too long");
        return;
    }
    int r = sys_mkdirat(AT_FDCWD, path, 0);
    if (r < 0) perror_("mkdir", r);
}

static void cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { putln("rmdir: missing path"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("rmdir: path too long");
        return;
    }
    /* FatFs's f_unlink handles directories (must be empty); we
     * pass AT_REMOVEDIR for clarity though FatFs ignores it. */
    int r = sys_unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
    if (r < 0) perror_("rmdir", r);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { putln("rm: missing path"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("rm: path too long");
        return;
    }
    int r = sys_unlinkat(AT_FDCWD, path, 0);
    if (r < 0) perror_("rm", r);
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { putln("touch: missing path"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("touch: path too long");
        return;
    }
    /* Open with O_CREAT and immediately close. If the file
     * already exists, this is a no-op. */
    int fd = sys_openat(AT_FDCWD, path, O_WRONLY | O_CREAT, 0);
    if (fd < 0) { perror_("touch", fd); return; }
    sys_close(fd);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { putln("cat: missing path"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("cat: path too long");
        return;
    }
    int fd = sys_openat(AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) { perror_("cat", fd); return; }

    char buf[256];
    for (;;) {
        int n = sys_read(fd, buf, sizeof(buf));
        if (n < 0) { perror_("cat", n); break; }
        if (n == 0) break;     /* EOF */
        sys_write(1, buf, (unsigned)n);
    }
    sys_close(fd);
}

static void cmd_write(int argc, char **argv) {
    if (argc < 3) { putln("write: usage: write <path> <text...>"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("write: path too long");
        return;
    }
    int fd = sys_openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) { perror_("write", fd); return; }

    /* Reassemble argv[2..] with space separators. */
    for (int i = 2; i < argc; i++) {
        if (i > 2) sys_write(fd, " ", 1);
        unsigned l = slen(argv[i]);
        int w = sys_write(fd, argv[i], l);
        if (w < 0) { perror_("write", w); sys_close(fd); return; }
    }
    sys_write(fd, "\n", 1);
    sys_close(fd);
}

/* ============================================================
 *  Main loop
 * ============================================================ */

static void dispatch(char *line) {
    char *argv[16];
    int argc = tokenize(line, argv, 16);
    if (argc == 0) return;       /* empty input */

    if      (scmp(argv[0], "help")  == 0) cmd_help();
    else if (scmp(argv[0], "pwd")   == 0) cmd_pwd();
    else if (scmp(argv[0], "ls")    == 0) cmd_ls(argc, argv);
    else if (scmp(argv[0], "cd")    == 0) cmd_cd(argc, argv);
    else if (scmp(argv[0], "mkdir") == 0) cmd_mkdir(argc, argv);
    else if (scmp(argv[0], "rmdir") == 0) cmd_rmdir(argc, argv);
    else if (scmp(argv[0], "rm")    == 0) cmd_rm(argc, argv);
    else if (scmp(argv[0], "touch") == 0) cmd_touch(argc, argv);
    else if (scmp(argv[0], "cat")   == 0) cmd_cat(argc, argv);
    else if (scmp(argv[0], "write") == 0) cmd_write(argc, argv);
    else if (scmp(argv[0], "exit")  == 0 || scmp(argv[0], "quit") == 0) {
        putln("bye");
        sys_exit(0);
    } else {
        puts_(argv[0]);
        putln(": unknown command (type 'help')");
    }
}

void _start(void) {
    putln("VM shell — type 'help' for commands");

    char line[LINE_CAP];
    int first = 1;
    for (;;) {
        /* Two-line prompt for breathing room and readability:
         *
         *   [<cwd>]
         *   $ <user input here>
         *
         * The blank line before the path separates each command's
         * output from the next prompt. The path on its own line
         * stays out of the way of long working directories. The
         * `$ ` on the input line keeps the cursor at a predictable
         * column regardless of cwd length.
         *
         * Skip the leading newline on the very first prompt — the
         * welcome banner already provides separation. */
        if (!first) puts_("\n");
        first = 0;
        puts_("[");
        puts_(g_cwd);
        puts_("]\n$ ");
        int n = readline(line, sizeof(line));
        if (n < 0) {
            putln("\n(stdin closed)");
            sys_exit(0);
        }
        dispatch(line);
    }
}
