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
#define SYS_FFLUSH     82
#define SYS_EXIT       93
#define SYS_READDIR   120
#define SYS_SLAB_STATS 1106
#define SYS_VM_STATS   1107
#define SYS_YIELD    1040
#define SYS_SPAWN_AND_WAIT  1104
#define SYS_TTY_SET_RAW     1105

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
#define E_PERM       1
#define E_IO         5
#define E_NOENT      2
#define E_BADF       9
#define E_EXIST     17
#define E_NOTDIR    20
#define E_ISDIR     21
#define E_INVAL     22
#define E_MFILE     24
#define E_NOSPC     28
#define E_ROFS      30
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
 *  Memory introspection (must mirror VmSlabStatsRecord and
 *  VmVmStatsRecord from include/vm/vm_ecall.h).
 * ============================================================ */

#define MEMINFO_BIN_COUNT 16

typedef struct {
    unsigned short version;
    unsigned short bin_count;
    unsigned int local_total;
    unsigned int local_in_use;
    unsigned int local_peak;
    unsigned int local_alloc_count;
    unsigned int local_free_count;
    unsigned int local_failed_count;
    unsigned int shared_total;
    unsigned int shared_in_use;
    unsigned int shared_peak;
    unsigned int shared_alloc_count;
    unsigned int shared_free_count;
    unsigned int shared_failed_count;
    unsigned int local_bins[MEMINFO_BIN_COUNT];
    unsigned int shared_bins[MEMINFO_BIN_COUNT];
} SlabStats;

typedef struct {
    unsigned short version;
    unsigned short vm_id;
    unsigned char  state;
    unsigned char  in_critical;
    unsigned char  alloc_count;
    unsigned char  _pad;
    unsigned int   text_bytes;
    unsigned int   rodata_bytes;
    unsigned int   data_bytes;
    unsigned int   mailbox_bytes;
    unsigned int   instructions_retired_lo;
    unsigned int   instructions_retired_hi;
    unsigned int   trap_count;
    unsigned int   ecall_count;
} VmStats;

static inline int sys_slab_stats(SlabStats *out) {
    register unsigned a0 asm("a0") = (unsigned)(unsigned long)out;
    register unsigned a1 asm("a1") = (unsigned)sizeof(SlabStats);
    register int      a7 asm("a7") = SYS_SLAB_STATS;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int)a0;
}

static inline int sys_vm_stats(int vm_id, VmStats *out) {
    register int      a0 asm("a0") = vm_id;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)out;
    register unsigned a2 asm("a2") = (unsigned)sizeof(VmStats);
    register int      a7 asm("a7") = SYS_VM_STATS;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

/* Spawn another ELF as a child VM and block until it exits.
 * Returns its exit code (0..255) on success, or a negative
 * errno on failure (e.g., -2 = ENOENT). The child shares this
 * VM's stdin/stdout/stderr — anything it prints appears on
 * the user's terminal interleaved with our own output. */
static inline int sys_spawn_and_wait(const char *path) {
    register int      a0 asm("a0") = (int)(unsigned long)path;
    register int      a7 asm("a7") = SYS_SPAWN_AND_WAIT;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_fflush(int fd) {
    register int a0 asm("a0") = fd;
    register int a7 asm("a7") = SYS_FFLUSH;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_tty_set_raw(int enable) {
    register int a0 asm("a0") = enable;
    register int a7 asm("a7") = SYS_TTY_SET_RAW;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
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

static char g_cwd[CWD_CAP] = "/td0";

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
        case E_PERM:        return "permission denied";
        case E_IO:          return "i/o error";
        case E_NOENT:       return "no such file or directory";
        case E_BADF:        return "bad file descriptor";
        case E_EXIST:       return "file exists";
        case E_NOTDIR:      return "not a directory";
        case E_ISDIR:       return "is a directory";
        case E_INVAL:       return "invalid argument";
        case E_MFILE:       return "too many open files";
        case E_NOSPC:       return "no space left on device";
        case E_ROFS:        return "read-only filesystem";
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

/* ============================================================
 *  Command history (ring buffer)
 *
 *  Fixed-size in-memory log of the last HIST_N submitted
 *  commands. Used by readline_raw for up/down recall.
 *
 *  Eviction is FIFO: when the buffer is full, adding a new
 *  command overwrites the oldest entry. Empty commands aren't
 *  added; consecutive duplicates are (matches sh, not bash with
 *  HISTIGNORE=dups).
 * ============================================================ */

#define HIST_N      16

static char     hist_buf[HIST_N][LINE_CAP];
static unsigned hist_count;     /* number of valid entries, ≤ HIST_N */
static unsigned hist_head;      /* index where the next entry lands */

static void history_add(const char *line) {
    if (!line || line[0] == '\0') return;
    scpy(hist_buf[hist_head], line, LINE_CAP);
    hist_head = (hist_head + 1) % HIST_N;
    if (hist_count < HIST_N) hist_count++;
}

/* Return the i-th-most-recent entry (i=0 is newest, i=count-1
 * is oldest), or NULL if i is out of range. */
static const char *history_get(unsigned i) {
    if (i >= hist_count) return 0;
    /* Most recent entry is at (hist_head - 1) mod HIST_N. */
    unsigned idx = (hist_head + HIST_N - 1 - i) % HIST_N;
    return hist_buf[idx];
}

/* ============================================================
 *  Raw-mode line editor
 *
 *  Replaces the cooked-mode readline. The shell puts the TTY
 *  into raw mode before calling this; the editor is responsible
 *  for echoing what the user types (the terminal doesn't auto-
 *  echo in raw mode) and for handling line editing — backspace,
 *  Ctrl-C, history recall via up/down.
 *
 *  Why raw mode at all: in cooked mode the terminal echoes every
 *  byte the user types, including ESC sequences from arrow keys.
 *  Echoing ESC [ A makes the terminal interpret it as 'cursor up'
 *  and visibly move the cursor away from the prompt. Raw mode
 *  disables echo so the editor can decide what's worth printing.
 *
 *  Supported keys (everything else is silently ignored):
 *
 *    Printable 0x20-0x7E    echo + append to buffer
 *    Backspace 0x7F or 0x08 if buffer non-empty: pop, erase
 *    Enter \n or \r         echo \r\n, return line
 *    Ctrl-C 0x03            echo ^C\r\n, return empty line
 *    Ctrl-D 0x04            if buffer empty: return EOF
 *    Ctrl-L 0x0C            clear screen, redraw prompt + line
 *    Ctrl-U 0x15            clear current line
 *    Up arrow (ESC [ A)     history: previous command
 *    Down arrow (ESC [ B)   history: next command (or restore draft)
 *
 *  Left/Right arrows are silently ignored — editing happens at
 *  the end of the line only. Same with function keys and other
 *  escape sequences.
 *
 *  Return value: line length (≥ 0) on Enter or Ctrl-C, -1 on
 *  Ctrl-D-at-empty or stdin error (caller treats as EOF).
 * ============================================================ */

/* The prompt the editor redraws when the line content changes
 * (e.g., on history recall, Ctrl-L, Ctrl-U). The outer loop
 * still prints the [<cwd>] line above, and writes this prompt
 * as the introductory text for the editor's row. */
static const char *EDIT_PROMPT = "$ ";

static void erase_line_and_redraw_prompt(void) {
    /* \r return to col 0; \x1b[2K erase entire line; prompt */
    puts_("\r\x1b[2K");
    puts_(EDIT_PROMPT);
}

/* Replace the editor's working buffer with `src`, redraw the
 * line to match. Used by history recall and Ctrl-U. */
static unsigned set_line(char *buf, const char *src) {
    erase_line_and_redraw_prompt();
    unsigned n = 0;
    if (src) {
        while (src[n] && n < LINE_CAP - 1) {
            buf[n] = src[n];
            n++;
        }
        buf[n] = '\0';
        sys_write(1, buf, n);
    } else {
        buf[0] = '\0';
    }
    sys_fflush(1);
    return n;
}

/* ESC-sequence state during input. */
typedef enum {
    ESC_NONE = 0,
    ESC_INTRO,         /* saw ESC, expecting [ or O */
    ESC_CSI,           /* saw ESC[ or ESCO, expecting final byte */
} EscState;

/* Sentinel value for hist_pos meaning "I'm composing a fresh
 * line, not viewing history." When the user presses Up from
 * this state, we snapshot the working buffer into `draft` so
 * Down can restore it. */
#define HIST_AT_DRAFT  ((unsigned)-1)

static int readline_raw(char *buf, unsigned cap) {
    unsigned pos = 0;
    EscState esc = ESC_NONE;
    unsigned hist_pos = HIST_AT_DRAFT;
    static char draft[LINE_CAP];
    draft[0] = '\0';

    buf[0] = '\0';
    sys_fflush(1);

    for (;;) {
        if (pos >= cap - 1) {
            /* Buffer full — treat next byte as Enter. */
            buf[cap - 1] = '\0';
            puts_("\r\n");
            sys_fflush(1);
            return (int)pos;
        }
        char c;
        int r = sys_read(0, &c, 1);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            sys_yield();
            continue;
        }

        /* ESC sequence dispatch. We resolve the sequence here
         * before processing as a normal byte, so that arrow keys
         * etc. are handled atomically. */
        if (esc == ESC_INTRO) {
            if (c == '[' || c == 'O') {
                esc = ESC_CSI;
            } else {
                esc = ESC_NONE;   /* short escape, ignore both bytes */
            }
            continue;
        }
        if (esc == ESC_CSI) {
            /* CSI final byte: 0x40-0x7E. Handle arrows; ignore
             * everything else (function keys, mouse, etc.). */
            if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
                if (c == 'A') {
                    /* Up: previous history entry */
                    if (hist_count > 0) {
                        unsigned new_pos;
                        if (hist_pos == HIST_AT_DRAFT) {
                            /* Snapshot the in-progress draft. */
                            scpy(draft, buf, LINE_CAP);
                            new_pos = 0;
                        } else if (hist_pos + 1 < hist_count) {
                            new_pos = hist_pos + 1;
                        } else {
                            new_pos = hist_pos;   /* clamp at oldest */
                        }
                        hist_pos = new_pos;
                        pos = set_line(buf, history_get(hist_pos));
                    }
                } else if (c == 'B') {
                    /* Down: next history entry or restore draft */
                    if (hist_pos != HIST_AT_DRAFT) {
                        if (hist_pos == 0) {
                            hist_pos = HIST_AT_DRAFT;
                            pos = set_line(buf, draft);
                        } else {
                            hist_pos--;
                            pos = set_line(buf, history_get(hist_pos));
                        }
                    }
                }
                /* C (right), D (left): ignore for now */
                esc = ESC_NONE;
            }
            /* Non-final byte: stay in ESC_CSI, keep collecting. */
            continue;
        }
        if (c == 0x1B) {
            esc = ESC_INTRO;
            continue;
        }

        /* Normal byte. */
        unsigned char b = (unsigned char)c;

        if (b == '\n' || b == '\r') {
            buf[pos] = '\0';
            puts_("\r\n");
            sys_fflush(1);
            return (int)pos;
        }
        if (b == 0x7F || b == 0x08) {     /* Backspace */
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                /* Erase visually: back up one column, overwrite
                 * with space, back up again. */
                puts_("\b \b");
                sys_fflush(1);
            }
            continue;
        }
        if (b == 0x03) {                  /* Ctrl-C */
            puts_("^C\r\n");
            sys_fflush(1);
            buf[0] = '\0';
            return 0;                     /* empty line, no history */
        }
        if (b == 0x04) {                  /* Ctrl-D */
            if (pos == 0) {
                return -1;                /* EOF on empty line */
            }
            continue;                     /* ignore on non-empty line */
        }
        if (b == 0x0C) {                  /* Ctrl-L: clear screen */
            puts_("\x1b[2J\x1b[H");       /* clear + home */
            erase_line_and_redraw_prompt();
            if (pos > 0) sys_write(1, buf, pos);
            sys_fflush(1);
            continue;
        }
        if (b == 0x15) {                  /* Ctrl-U: clear line */
            pos = 0;
            buf[0] = '\0';
            erase_line_and_redraw_prompt();
            sys_fflush(1);
            continue;
        }
        if (b >= 0x20 && b <= 0x7E) {     /* Printable */
            buf[pos++] = (char)b;
            buf[pos] = '\0';
            sys_write(1, &c, 1);
            sys_fflush(1);
            continue;
        }
        /* Any other control byte: silently ignore. */
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
    putln("  cp <src> <dst>       copy file");
    putln("  mv <src> <dst>       move/rename file");
    putln("  run <path>           explicitly load and execute an ELF");
    putln("  <path>               same as run; e.g. /host/snake.elf");
    putln("  history              show recent commands");
    putln("  meminfo              show slab allocator + per-VM stats");
    putln("  help                 this message");
    putln("  exit                 leave shell");
    putln("");
    putln("editor keys: backspace, up/down (history),");
    putln("             Ctrl-L (clear screen), Ctrl-U (clear line),");
    putln("             Ctrl-C (cancel line), Ctrl-D (exit on empty line)");
}

static void cmd_history(void) {
    /* Print oldest to newest. history_get(i) gives the i-th-most-
     * recent, so we walk i from count-1 down to 0. The displayed
     * line number is i+1 from the top (1-indexed, oldest first). */
    if (hist_count == 0) {
        putln("(history is empty)");
        return;
    }
    for (unsigned i = hist_count; i-- > 0; ) {
        const char *line = history_get(i);
        if (!line) continue;
        char buf[12];
        char *p = fmt_u32(hist_count - i, buf + sizeof(buf));
        /* right-pad to 4 columns for tidy alignment */
        unsigned w = (unsigned)((buf + sizeof(buf) - 1) - p);
        for (unsigned k = w; k < 4; k++) puts_(" ");
        puts_(p);
        puts_("  ");
        putln(line);
    }
}

/* Print a number right-aligned in W columns. Truncates at 24 digits. */
static void put_num_right(unsigned v, unsigned width) {
    char buf[24];
    char *p = fmt_u32(v, buf + sizeof(buf));
    unsigned len = (unsigned)((buf + sizeof(buf) - 1) - p);
    while (len < width) { puts_(" "); width--; }
    puts_(p);
}

/* Print a byte count with a 'K' suffix when ≥ 1024, else as bytes. */
static void put_bytes(unsigned v, unsigned width) {
    char buf[24];
    if (v >= 1024) {
        /* Build the digit string, then append 'K' just before the
         * null. fmt_u32 writes the terminator at buf_end-1 and the
         * first digit somewhere earlier; we slide a 'K' in by
         * formatting one byte before the end and overwriting the
         * resulting null. */
        char *p = fmt_u32(v / 1024, buf + sizeof(buf) - 1);
        /* buf_end-2 is where the null currently sits (since the
         * helper was given buf+sizeof-1 as buf_end). Replace with
         * 'K', re-null at the next slot. */
        buf[sizeof(buf) - 2] = 'K';
        buf[sizeof(buf) - 1] = '\0';
        unsigned len = (unsigned)((buf + sizeof(buf) - 1) - p);
        while (len < width) { puts_(" "); width--; }
        puts_(p);
    } else {
        put_num_right(v, width);
    }
}

static void cmd_meminfo(void) {
    SlabStats st;
    int n = sys_slab_stats(&st);
    if (n < 0) {
        putln("meminfo: SYS_SLAB_STATS failed");
        return;
    }

    /* Summary lines. */
    putln("slab allocators:");
    puts_("  local  ");
    put_bytes(st.local_in_use, 6);
    puts_(" / ");
    put_bytes(st.local_total, 6);
    puts_("   peak ");
    put_bytes(st.local_peak, 6);
    puts_("   allocs ");
    put_num_right(st.local_alloc_count, 4);
    puts_("   frees ");
    put_num_right(st.local_free_count, 4);
    putln("");

    puts_("  shared ");
    put_bytes(st.shared_in_use, 6);
    puts_(" / ");
    put_bytes(st.shared_total, 6);
    puts_("   peak ");
    put_bytes(st.shared_peak, 6);
    puts_("   allocs ");
    put_num_right(st.shared_alloc_count, 4);
    puts_("   frees ");
    put_num_right(st.shared_free_count, 4);
    putln("");

    /* Per-bin breakdown for non-empty bins. */
    putln("");
    putln("bins (size | local in_use/count | shared in_use/count):");
    for (unsigned i = 0; i < MEMINFO_BIN_COUNT; i++) {
        unsigned lc = st.local_bins[i] & 0xFFFFu;
        unsigned li = (st.local_bins[i] >> 16) & 0xFFFFu;
        unsigned sc = st.shared_bins[i] & 0xFFFFu;
        unsigned si = (st.shared_bins[i] >> 16) & 0xFFFFu;
        if (lc == 0 && sc == 0) continue;
        unsigned block = 32u << i;
        puts_("  ");
        put_bytes(block, 6);
        puts_("   ");
        put_num_right(li, 3);
        puts_(" / ");
        put_num_right(lc, 3);
        puts_("           ");
        put_num_right(si, 3);
        puts_(" / ");
        put_num_right(sc, 3);
        putln("");
    }

    /* Per-VM info. Iterate from 0 until we hit a not-found. */
    putln("");
    putln("VMs (id  text   rodata data   mbox   allocs):");
    for (int id = 0; id < 16; id++) {
        VmStats vs;
        int r = sys_vm_stats(id, &vs);
        if (r < 0) continue;          /* slot empty */
        puts_("  ");
        put_num_right((unsigned)vs.vm_id, 2);
        puts_("   ");
        put_bytes(vs.text_bytes, 5);
        puts_("  ");
        put_bytes(vs.rodata_bytes, 5);
        puts_("  ");
        put_bytes(vs.data_bytes, 5);
        puts_("  ");
        put_bytes(vs.mailbox_bytes, 5);
        puts_("  ");
        put_num_right(vs.alloc_count, 4);
        putln("");
    }
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

/* Copy bytes from one open fd to another. Returns 0 on success
 * or a negative errno from the failing read/write. */
static int copy_fd_to_fd(int src_fd, int dst_fd) {
    char buf[512];
    for (;;) {
        int n = sys_read(src_fd, buf, sizeof(buf));
        if (n < 0) return n;
        if (n == 0) return 0;        /* EOF */
        int written = 0;
        while (written < n) {
            int w = sys_write(dst_fd, buf + written, (unsigned)(n - written));
            if (w < 0) return w;
            if (w == 0) return -5;     /* EIO — shouldn't happen on files */
            written += w;
        }
    }
}

/* cp src dst
 *
 * Copies one file to another. Truncates dst if it exists.
 * Works across mounts (e.g., cp /host/foo.elf /td0/foo.elf)
 * since the read/write loop is backend-agnostic. */
static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { putln("cp: usage: cp <src> <dst>"); return; }
    char src[PATH_CAP], dst[PATH_CAP];
    if (resolve_path(argv[1], src) != 0) {
        putln("cp: src path too long");
        return;
    }
    if (resolve_path(argv[2], dst) != 0) {
        putln("cp: dst path too long");
        return;
    }

    int sfd = sys_openat(AT_FDCWD, src, O_RDONLY, 0);
    if (sfd < 0) { perror_("cp", sfd); return; }

    int dfd = sys_openat(AT_FDCWD, dst, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (dfd < 0) { perror_("cp", dfd); sys_close(sfd); return; }

    int r = copy_fd_to_fd(sfd, dfd);
    sys_close(sfd);
    sys_close(dfd);
    if (r < 0) perror_("cp", r);
}

/* mv src dst
 *
 * Moves (renames) a file. Implementation: copy src to dst,
 * then unlink src. If the unlink fails after a successful
 * copy, the file ends up duplicated rather than moved, and
 * we print a warning so the user knows to clean up.
 *
 * A future commit could add a SYS_RENAME for the same-mount
 * case (which FatFs can do via f_rename and the host fs via
 * rename(2)), but cp+unlink works as a portable fallback
 * for all our cases today. */
static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { putln("mv: usage: mv <src> <dst>"); return; }
    char src[PATH_CAP], dst[PATH_CAP];
    if (resolve_path(argv[1], src) != 0) {
        putln("mv: src path too long");
        return;
    }
    if (resolve_path(argv[2], dst) != 0) {
        putln("mv: dst path too long");
        return;
    }

    int sfd = sys_openat(AT_FDCWD, src, O_RDONLY, 0);
    if (sfd < 0) { perror_("mv", sfd); return; }

    int dfd = sys_openat(AT_FDCWD, dst, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (dfd < 0) { perror_("mv", dfd); sys_close(sfd); return; }

    int r = copy_fd_to_fd(sfd, dfd);
    sys_close(sfd);
    sys_close(dfd);
    if (r < 0) {
        perror_("mv", r);
        /* Don't unlink src on failed copy — caller may still
         * have the original. */
        return;
    }

    int u = sys_unlinkat(AT_FDCWD, src, 0);
    if (u < 0) {
        puts_("mv: warning — copied to dst but failed to remove src: ");
        perror_("mv", u);
    }
}

/* run <path>
 *
 * Spawn an ELF file as a child VM and wait for it to exit.
 * The child shares our stdio, so anything it prints appears
 * on the user's terminal interleaved with our own output.
 *
 * The path can be:
 *   - On the FatFs RAM volume (any path that doesn't start with /host)
 *   - On the host filesystem (under /host/, e.g., /host/calc.elf)
 *
 * Prints the child's exit code only if it's non-zero (so the
 * common success case is quiet, like Unix shells). */
static void exec_path(const char *path);   /* defined below */

static void cmd_run(int argc, char **argv) {
    if (argc < 2) { putln("run: usage: run <path>"); return; }
    char path[PATH_CAP];
    if (resolve_path(argv[1], path) != 0) {
        putln("run: path too long");
        return;
    }
    exec_path(path);
}

/* ============================================================
 *  Cursor shape (DECSCUSR — DEC Set Cursor Shape)
 *
 *  Modern terminals (mintty, xterm, iTerm2, Windows Terminal,
 *  Konsole, gnome-terminal) implement DECSCUSR for runtime
 *  cursor-style selection. The escape is CSI Ps SP q where:
 *
 *    Ps = 0  reset to terminal default
 *    Ps = 1  blinking block
 *    Ps = 2  steady block
 *    Ps = 3  blinking underline
 *    Ps = 4  steady underline
 *    Ps = 5  blinking bar
 *    Ps = 6  steady bar
 *
 *  We use steady block — thin bars are hard to spot at the
 *  prompt position. The reset-to-default on exit restores
 *  whatever the user configured at the terminal level.
 *
 *  This sequence is harmless on terminals that don't recognise
 *  it — they treat unknown escapes as a no-op or print nothing.
 * ============================================================ */

static void cursor_steady_block(void)   { puts_("\x1b[2 q"); }
static void cursor_reset_default(void)  { puts_("\x1b[0 q"); }

/* ============================================================
 *  Main loop
 * ============================================================ */

/* Return non-zero if `tok` looks like a path the shell should
 * try to execute directly. Two cases:
 *
 *   1. The token contains a '/' → it's an explicit path. Always
 *      attempt to run it (resolves relative to cwd if it doesn't
 *      start with '/'). If the file doesn't exist, the spawn
 *      call returns -ENOENT and the caller prints a clear error.
 *
 *   2. The token has no '/' → resolve it against cwd. If a file
 *      by that name exists in cwd, treat it as a path; otherwise
 *      let the builtin lookup handle it. This way a bare token
 *      like 'snake.elf' inside /host runs the file, but 'ls'
 *      anywhere keeps invoking the builtin (builtins win on
 *      name conflicts because they're checked first below).
 *
 * `path_out` is set to the resolved absolute path when this
 * returns non-zero. */
static int looks_like_runnable_path(const char *tok, char *path_out) {
    if (resolve_path(tok, path_out) != 0) return 0;
    /* Test existence by trying to open read-only. */
    int fd = sys_openat(AT_FDCWD, path_out, O_RDONLY, 0);
    if (fd < 0) return 0;
    sys_close(fd);
    return 1;
}

/* Run an already-resolved guest path as a child VM and report
 * the result. Factored out of cmd_run so the path-first
 * dispatcher can share it. */
static void exec_path(const char *path) {
    int rc = sys_spawn_and_wait(path);
    if (rc < 0) {
        perror_("run", rc);
        return;
    }
    if (rc != 0) {
        char buf[16];
        char *p = fmt_u32((unsigned)rc, buf + sizeof(buf));
        puts_("run: exit ");
        puts_(p);
        putln("");
    }
}

static void dispatch(char *line) {
    char *argv[16];
    int argc = tokenize(line, argv, 16);
    if (argc == 0) return;       /* empty input */

    /* Builtins win on name conflicts (Unix-like predictability). */
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
    else if (scmp(argv[0], "cp")    == 0) cmd_cp(argc, argv);
    else if (scmp(argv[0], "mv")    == 0) cmd_mv(argc, argv);
    else if (scmp(argv[0], "run")   == 0) cmd_run(argc, argv);
    else if (scmp(argv[0], "history") == 0) cmd_history();
    else if (scmp(argv[0], "meminfo") == 0) cmd_meminfo();
    else if (scmp(argv[0], "exit")  == 0 || scmp(argv[0], "quit") == 0) {
        putln("bye");
        cursor_reset_default();
        sys_tty_set_raw(0);             /* leave terminal cooked */
        sys_exit(0);
    } else {
        /* Not a builtin. If the token resolves to a file we can
         * open, run it as a guest ELF (path-first command form).
         * Otherwise print "unknown command". */
        char path[PATH_CAP];
        if (looks_like_runnable_path(argv[0], path)) {
            exec_path(path);
        } else {
            puts_(argv[0]);
            putln(": unknown command (type 'help')");
        }
    }
}


void _start(void) {
    /* Set a steady block cursor — thin bars are hard to spot at
     * the prompt position, and our shell doesn't currently use
     * mid-line cursor movement for editing. The terminal-default
     * reset happens on every exit path. */
    cursor_steady_block();

    /* Enter raw mode for the editor. From this point on we own
     * line editing — backspace, echo, arrow keys, etc. The flag
     * tells us whether the toggle actually engaged (it returns
     * false when stdin isn't a TTY, e.g., during automated test
     * runs that pipe input). */
    int have_raw = (sys_tty_set_raw(1) == 0);

    putln("VM shell -- type 'help' for commands");

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
         * stays out of the way of long working directories.
         *
         * Skip the leading newline on the very first prompt — the
         * welcome banner already provides separation. */
        if (!first) puts_("\r\n");
        first = 0;
        puts_("[");
        puts_(g_cwd);
        puts_("]\r\n");
        /* The `$ ` prompt itself is printed by readline_raw, so
         * that history recall and Ctrl-U redraws can rebuild the
         * same prompt-then-line layout. */
        puts_("$ ");
        sys_fflush(1);

        int n = readline_raw(line, sizeof(line));
        if (n < 0) {
            puts_("\r\n");
            putln("(stdin closed)");
            cursor_reset_default();
            if (have_raw) sys_tty_set_raw(0);
            sys_exit(0);
        }
        /* Re-enter raw mode after dispatch — if dispatch ran a
         * spawn, the child may have toggled raw mode on its way
         * out (snake does this politely). We want it on for our
         * own prompt. */
        history_add(line);
        dispatch(line);
        if (have_raw) sys_tty_set_raw(1);
    }
}
