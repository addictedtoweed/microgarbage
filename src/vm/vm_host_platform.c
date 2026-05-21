/* ============================================================
 *  vm_host_platform.c — small platform services
 *  See vm/vm_host_platform.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_host_platform.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"
#include "vm/vm_host_stdio.h"
#include "memory/slab_stack.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 *  Module state
 * ============================================================ */

static VmRealtimeSource g_realtime_source = NULL;
static void            *g_realtime_userdata = NULL;

/* SplitMix64 state. Initialized from rand_seed at install time. */
static uint64_t         g_rand_state = 0x9E3779B97F4A7C15ULL;

/* ============================================================
 *  Internal helpers
 * ============================================================ */

/* Find the slab block size for a SYS_ALLOC pointer. The shared
 * slab tags each block with the bin index in its header; the bin
 * width gives the size. Returns the usable size, or 0 if `ptr`
 * isn't a slab block. */
static size_t shared_alloc_size(VmSystem *sys, void *ptr) {
    if (!sys || !ptr) return 0;
    /* The slab API exposes slab_block_size(allocator, ptr) for
     * exactly this. We assume it's present. */
    return slab_block_size(sys->shared_slab, ptr);
}

/* ============================================================
 *  Formatter — used by both SYS_FORMAT_AND_WRITE and SYS_FORMAT_TO_BUF.
 *
 *  fmt is the format string (already copied into host memory),
 *  args[] is the array of u32 args (already pulled from guest),
 *  argc is the count. The output is appended to *pout (a writer
 *  callback so we can target either a fd or a buffer).
 * ============================================================ */

typedef struct {
    /* Callback to emit bytes. Returns true to continue, false on
     * error or capacity reached. Buffer-mode tracks bytes written
     * regardless of cap; fd-mode writes via write(). */
    bool (*emit)(void *out_ctx, const char *bytes, size_t n);
    void   *out_ctx;
    size_t  emitted;       /* total bytes emit() was given */
    bool    failed;
} FmtSink;

static void fmt_emit(FmtSink *s, const char *bytes, size_t n) {
    if (s->failed) return;
    if (n == 0) return;
    if (!s->emit(s->out_ctx, bytes, n)) {
        s->failed = true;
        return;
    }
    s->emitted += n;
}

static void fmt_emit_char(FmtSink *s, char c) {
    fmt_emit(s, &c, 1);
}

static void fmt_pad(FmtSink *s, char c, int n) {
    char buf[16];
    if (n <= 0) return;
    memset(buf, c, sizeof(buf));
    while (n >= (int)sizeof(buf)) {
        fmt_emit(s, buf, sizeof(buf));
        n -= (int)sizeof(buf);
    }
    if (n > 0) fmt_emit(s, buf, (size_t)n);
}

/* Format an unsigned integer in base `base` into the END of buf.
 * Returns a pointer to the first character of the resulting
 * string. Does NOT null-terminate. */
static char *fmt_uint(uint32_t v, unsigned base, bool upper,
                      char *buf_end) {
    char *p = buf_end;
    if (v == 0) {
        *--p = '0';
        return p;
    }
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) {
        *--p = digits[v % base];
        v /= base;
    }
    return p;
}

/* The actual formatter.
 *
 * Returns 0 on success; -1 on emit failure (sink-imposed cap).
 * `*out_written` always reflects how many bytes the formatter
 * tried to emit, even on failure — which is how snprintf-style
 * "would have written N" semantics are conveyed up. */
static int format_va(FmtSink *sink,
                     const char *fmt,
                     const uint32_t *args, unsigned argc,
                     /* For %s arg dereferencing, the formatter needs
                      * to read guest memory. The cpu pointer gives it
                      * vm_translate_read access. */
                     VmCpu *cpu) {
    unsigned argi = 0;

    for (const char *p = fmt; *p; ) {
        if (*p != '%') {
            const char *start = p;
            while (*p && *p != '%') p++;
            fmt_emit(sink, start, (size_t)(p - start));
            continue;
        }
        p++;  /* skip '%' */

        /* Flags */
        bool flag_minus = false, flag_zero = false;
        bool flag_plus  = false, flag_space = false, flag_hash = false;
        while (*p == '-' || *p == '0' || *p == '+' ||
               *p == ' ' || *p == '#') {
            switch (*p) {
                case '-': flag_minus = true; break;
                case '0': flag_zero  = true; break;
                case '+': flag_plus  = true; break;
                case ' ': flag_space = true; break;
                case '#': flag_hash  = true; break;
            }
            p++;
        }

        /* Width */
        int width = 0;
        if (*p == '*') {
            if (argi < argc) width = (int)args[argi++];
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }

        /* Precision */
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                if (argi < argc) precision = (int)args[argi++];
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
        }

        /* Ignore length modifiers — guests on RV32 don't have
         * meaningful long-vs-int distinction. */
        while (*p == 'h' || *p == 'l' || *p == 'z' ||
               *p == 't' || *p == 'j') {
            p++;
        }

        char conv = *p;
        if (!conv) break;
        p++;

        switch (conv) {
            case '%': fmt_emit_char(sink, '%'); break;

            case 'c': {
                if (argi >= argc) { fmt_emit_char(sink, '?'); break; }
                int c = (int)args[argi++];
                if (!flag_minus) fmt_pad(sink, ' ', width - 1);
                fmt_emit_char(sink, (char)c);
                if (flag_minus) fmt_pad(sink, ' ', width - 1);
                break;
            }

            case 's': {
                if (argi >= argc) { fmt_emit(sink, "(null)", 6); break; }
                uint32_t guest_ptr = args[argi++];
                /* Read up to VM_FORMAT_MAX_STR_ARG bytes (or until null)
                 * from guest memory. */
                if (guest_ptr == 0) {
                    /* Treat null pointer like libc's printf("%s", NULL):
                     * emit "(null)". */
                    const char *literal = "(null)";
                    int len = 6;
                    if (precision >= 0 && precision < len) len = precision;
                    if (!flag_minus) fmt_pad(sink, ' ', width - len);
                    fmt_emit(sink, literal, (size_t)len);
                    if (flag_minus) fmt_pad(sink, ' ', width - len);
                    break;
                }
                /* Find length, capped. */
                int len = 0;
                while (len < VM_FORMAT_MAX_STR_ARG) {
                    const uint8_t *b = vm_translate_read(cpu, guest_ptr + (uint32_t)len, 1);
                    if (!b) break;                /* bad ptr — truncate */
                    if (*b == '\0') break;
                    if (precision >= 0 && len >= precision) break;
                    len++;
                }
                if (!flag_minus) fmt_pad(sink, ' ', width - len);
                /* Stream out the bytes. */
                for (int i = 0; i < len; i++) {
                    const uint8_t *b = vm_translate_read(cpu, guest_ptr + (uint32_t)i, 1);
                    if (!b) break;
                    fmt_emit_char(sink, (char)*b);
                }
                if (flag_minus) fmt_pad(sink, ' ', width - len);
                break;
            }

            case 'd':
            case 'i': {
                if (argi >= argc) { fmt_emit_char(sink, '?'); break; }
                int32_t signed_v = (int32_t)args[argi++];
                uint32_t v;
                char sign = 0;
                if (signed_v < 0) {
                    v = (uint32_t)(-(int64_t)signed_v);
                    sign = '-';
                } else {
                    v = (uint32_t)signed_v;
                    if (flag_plus)       sign = '+';
                    else if (flag_space) sign = ' ';
                }
                char buf[16];
                char *digits = fmt_uint(v, 10, false, buf + sizeof(buf));
                int  dlen    = (int)((buf + sizeof(buf)) - digits);
                int  numlen  = (sign ? 1 : 0) + dlen;
                int  pad     = width - numlen;
                if (precision > dlen) {
                    /* precision pads with zeros inside the number */
                    int prec_zeros = precision - dlen;
                    pad -= prec_zeros;
                    if (!flag_minus && !flag_zero) fmt_pad(sink, ' ', pad);
                    if (sign) fmt_emit_char(sink, sign);
                    fmt_pad(sink, '0', prec_zeros);
                    fmt_emit(sink, digits, (size_t)dlen);
                    if (flag_minus) fmt_pad(sink, ' ', pad);
                } else {
                    if (!flag_minus && !flag_zero) fmt_pad(sink, ' ', pad);
                    if (sign) fmt_emit_char(sink, sign);
                    if (!flag_minus && flag_zero && precision < 0)
                        fmt_pad(sink, '0', pad);
                    fmt_emit(sink, digits, (size_t)dlen);
                    if (flag_minus) fmt_pad(sink, ' ', pad);
                }
                break;
            }

            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                if (argi >= argc) { fmt_emit_char(sink, '?'); break; }
                uint32_t v = args[argi++];
                unsigned base = 10;
                bool upper = false;
                const char *prefix = "";
                int prefix_len = 0;
                if (conv == 'x') { base = 16; upper = false;
                    if (flag_hash && v) { prefix = "0x"; prefix_len = 2; } }
                if (conv == 'X') { base = 16; upper = true;
                    if (flag_hash && v) { prefix = "0X"; prefix_len = 2; } }
                if (conv == 'o') { base = 8;
                    if (flag_hash && v) { prefix = "0";  prefix_len = 1; } }
                char buf[16];
                char *digits = fmt_uint(v, base, upper, buf + sizeof(buf));
                int  dlen    = (int)((buf + sizeof(buf)) - digits);
                int  numlen  = prefix_len + dlen;
                int  pad     = width - numlen;
                if (precision > dlen) {
                    int prec_zeros = precision - dlen;
                    pad -= prec_zeros;
                    if (!flag_minus && !flag_zero) fmt_pad(sink, ' ', pad);
                    fmt_emit(sink, prefix, (size_t)prefix_len);
                    fmt_pad(sink, '0', prec_zeros);
                    fmt_emit(sink, digits, (size_t)dlen);
                    if (flag_minus) fmt_pad(sink, ' ', pad);
                } else {
                    if (!flag_minus && !flag_zero) fmt_pad(sink, ' ', pad);
                    fmt_emit(sink, prefix, (size_t)prefix_len);
                    if (!flag_minus && flag_zero && precision < 0)
                        fmt_pad(sink, '0', pad);
                    fmt_emit(sink, digits, (size_t)dlen);
                    if (flag_minus) fmt_pad(sink, ' ', pad);
                }
                break;
            }

            case 'p': {
                /* No width/precision handling for %p — always 8 hex chars,
                 * no prefix. RV32 pointer is 32 bits. */
                if (argi >= argc) { fmt_emit_char(sink, '?'); break; }
                uint32_t v = args[argi++];
                char buf[16];
                char *digits = fmt_uint(v, 16, false, buf + sizeof(buf));
                int dlen = (int)((buf + sizeof(buf)) - digits);
                fmt_pad(sink, '0', 8 - dlen);
                fmt_emit(sink, digits, (size_t)dlen);
                break;
            }

            default:
                /* Unknown conversion — echo it literally. */
                fmt_emit_char(sink, '%');
                fmt_emit_char(sink, conv);
                break;
        }
    }

    return sink->failed ? -1 : 0;
}

/* ============================================================
 *  Sink: fd
 * ============================================================ */

typedef struct {
    int fd;
} FdSinkCtx;

static bool fd_emit(void *ctx, const char *bytes, size_t n) {
    FdSinkCtx *c = (FdSinkCtx *)ctx;
    /* Use write(2). fd 1/2 go to host stdout/stderr; file fds go
     * through the platform's own write... actually no — fds >= 3
     * belong to vm_host_fs. The cleanest is to use the platform-level
     * write() which the host has installed for stdout/stderr (fd 1/2).
     * For files, the guest should be using SYS_WRITE directly anyway.
     * In practice printf targets 1 or 2; we honor that.
     *
     * For now we go direct: fd 1 → host stdout, fd 2 → host stderr,
     * other fds → -EINVAL via the caller. */
    size_t total = 0;
    while (total < n) {
        ssize_t w = write(c->fd, bytes + total, n - total);
        if (w < 0) return false;
        if (w == 0) return false;
        total += (size_t)w;
    }
    return true;
}

/* ============================================================
 *  Sink: buffer
 * ============================================================ */

typedef struct {
    char  *buf;
    size_t cap;        /* writable bytes (excluding null) */
    size_t pos;        /* bytes written so far (capped at cap) */
} BufSinkCtx;

static bool buf_emit(void *ctx, const char *bytes, size_t n) {
    BufSinkCtx *c = (BufSinkCtx *)ctx;
    /* snprintf semantics: write at most cap bytes total, always
     * leaving room for a trailing null. Bytes that don't fit are
     * dropped but emitted bytes are still counted (the formatter
     * tracks that via sink->emitted). */
    if (c->pos < c->cap) {
        size_t room = c->cap - c->pos;
        size_t write_n = n < room ? n : room;
        memcpy(c->buf + c->pos, bytes, write_n);
        c->pos += write_n;
    }
    return true;  /* never fail — we just drop overflow */
}

/* ============================================================
 *  Handler: SYS_FORMAT_AND_WRITE
 * ============================================================ */

/* Copy a guest string into a host buffer, capped at `cap` bytes
 * (including terminator). Returns the strlen on success, or -1
 * on bad guest pointer / too long. */
static int copy_guest_string(VmCpu *cpu, uint32_t guest_ptr,
                             char *out, size_t cap) {
    if (cap == 0) return -1;
    size_t i = 0;
    for (;;) {
        if (i + 1 >= cap) return -1;        /* too long */
        const uint8_t *b = vm_translate_read(cpu, guest_ptr + (uint32_t)i, 1);
        if (!b) return -1;                  /* bad guest ptr */
        out[i] = (char)*b;
        if (*b == '\0') return (int)i;
        i++;
    }
}

/* Copy guest u32 args array. */
static int copy_guest_args(VmCpu *cpu, uint32_t guest_ptr, unsigned argc,
                           uint32_t *out) {
    if (argc > VM_FORMAT_MAX_ARGS) return -1;
    if (argc == 0) return 0;
    if (guest_ptr == 0) return -1;
    /* Try to grab the whole array in one translate. If it doesn't
     * fit a single region, fall back to byte-by-byte (rare). */
    size_t need = (size_t)argc * sizeof(uint32_t);
    const void *p = vm_translate_read(cpu, guest_ptr, need);
    if (p) {
        memcpy(out, p, need);
        return 0;
    }
    /* Fallback: u32-by-u32. */
    for (unsigned i = 0; i < argc; i++) {
        const uint32_t *w = (const uint32_t *)vm_translate_read(
            cpu, guest_ptr + i * 4u, 4);
        if (!w) return -1;
        out[i] = *w;
    }
    return 0;
}

static void handle_format_and_write(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t fd          = cpu->regs[VM_REG_A0];
    uint32_t fmt_ptr     = cpu->regs[VM_REG_A1];
    uint32_t args_ptr    = cpu->regs[VM_REG_A2];
    uint32_t argc        = cpu->regs[VM_REG_A3];

    /* Only fd 1 and 2 are supported here. File-fd writes should
     * go through SYS_WRITE (the FS bridge), not the formatter. */
    if (fd != 1 && fd != 2) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        return;
    }

    char fmt[VM_FORMAT_MAX_FMT_LEN];
    if (copy_guest_string(cpu, fmt_ptr, fmt, sizeof(fmt)) < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    uint32_t args[VM_FORMAT_MAX_ARGS];
    if (copy_guest_args(cpu, args_ptr, argc, args) < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    FdSinkCtx ctx = { .fd = (int)fd };
    FmtSink sink = {
        .emit = fd_emit, .out_ctx = &ctx,
        .emitted = 0, .failed = false,
    };
    format_va(&sink, fmt, args, argc, cpu);

    if (sink.failed) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
    } else {
        cpu->regs[VM_REG_A0] = (uint32_t)sink.emitted;
    }
}

/* ============================================================
 *  Handler: SYS_FORMAT_TO_BUF
 * ============================================================ */

static void handle_format_to_buf(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t buf_ptr     = cpu->regs[VM_REG_A0];
    uint32_t cap         = cpu->regs[VM_REG_A1];
    uint32_t fmt_ptr     = cpu->regs[VM_REG_A2];
    uint32_t args_ptr    = cpu->regs[VM_REG_A3];
    uint32_t argc        = cpu->regs[VM_REG_A4];

    if (cap == 0) {
        /* Match snprintf(NULL, 0, ...) semantics — return the
         * count we'd have written. */
    }

    char fmt[VM_FORMAT_MAX_FMT_LEN];
    if (copy_guest_string(cpu, fmt_ptr, fmt, sizeof(fmt)) < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    uint32_t args[VM_FORMAT_MAX_ARGS];
    if (copy_guest_args(cpu, args_ptr, argc, args) < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    /* Get a writable host pointer for the guest buffer. We cap
     * the host-visible length to keep the translation small.
     * If cap > 0, we'll later null-terminate the in-bounds part. */
    char *host_buf = NULL;
    size_t host_cap = 0;
    if (cap > 0 && buf_ptr != 0) {
        /* Translate up to cap bytes. Some hosts won't return a
         * region larger than a page; in that case we'd need a
         * scatter path. For now we expect cap to be modest
         * (printf buffers are usually small). */
        size_t want = cap;
        if (want > 65536) want = 65536;       /* sane upper bound */
        host_buf = (char *)vm_translate_write(cpu, buf_ptr, want);
        if (!host_buf) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
            return;
        }
        /* Leave room for null. */
        host_cap = want - 1;
    }

    BufSinkCtx ctx = { .buf = host_buf, .cap = host_cap, .pos = 0 };
    FmtSink sink = {
        .emit = buf_emit, .out_ctx = &ctx,
        .emitted = 0, .failed = false,
    };
    format_va(&sink, fmt, args, argc, cpu);

    /* Null-terminate if there's any room. */
    if (host_buf && cap > 0) {
        size_t term_at = ctx.pos < host_cap ? ctx.pos : host_cap;
        host_buf[term_at] = '\0';
    }

    cpu->regs[VM_REG_A0] = (uint32_t)sink.emitted;
}

/* ============================================================
 *  Handler: SYS_REALTIME_NOW
 * ============================================================ */

static void handle_realtime_now(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t out_ptr = cpu->regs[VM_REG_A0];

    if (!g_realtime_source) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOSYS;
        return;
    }

    uint32_t seconds = 0, nanos = 0;
    if (!g_realtime_source(g_realtime_userdata, &seconds, &nanos)) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOSYS;
        return;
    }

    VmRealtimeRecord *rec = (VmRealtimeRecord *)vm_translate_write(
        cpu, out_ptr, sizeof(VmRealtimeRecord));
    if (!rec) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }
    rec->version = 1;
    rec->seconds = seconds;
    rec->nanos   = nanos;
    cpu->regs[VM_REG_A0] = 0;
}

/* ============================================================
 *  Handler: SYS_ALLOC_SIZE
 * ============================================================ */

static void handle_alloc_size(VmCpu *cpu, void *system) {
    VmSystem *sys = (VmSystem *)system;
    uint32_t guest_ptr = cpu->regs[VM_REG_A0];

    if (guest_ptr == 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }

    /* Translate to host pointer. We do a 1-byte read because we
     * only need the pointer's region, not actual data. */
    const uint8_t *host_ptr = vm_translate_read(cpu, guest_ptr, 1);
    if (!host_ptr) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    size_t sz = shared_alloc_size(sys, (void *)host_ptr);
    if (sz == 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }
    cpu->regs[VM_REG_A0] = (uint32_t)sz;
}

/* ============================================================
 *  Handler: SYS_RAND
 * ============================================================ */

/* SplitMix64 — one of the simplest, fastest PRNGs with good
 * distribution properties. Not cryptographic. State is a single
 * u64; output is the high 32 bits of each step. */
static uint32_t splitmix64_next32(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (uint32_t)(z >> 32);
}

static void handle_rand(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = splitmix64_next32(&g_rand_state);
}

/* ============================================================
 *  Handler: SYS_TIMING_DEADLINE_REMAINING
 * ============================================================ */

static void handle_timing_deadline_remaining(VmCpu *cpu, void *system) {
    VmSystem *sys = (VmSystem *)system;
    if (!sys || !sys->sched) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }
    /* The VM's reload deadline is in cpu->reload_next_deadline.
     * Return (deadline - now), clamped at 0 if we've already
     * passed it. If no reload period is set, return 0 — the
     * guest is free to do unbounded work. */
    if (cpu->reload_period == 0) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }
    uint32_t now = sys->sched->global_tick;
    uint32_t dl  = cpu->reload_next_deadline;
    /* Signed subtraction for proper wrap-aware comparison. */
    int32_t delta = (int32_t)(dl - now);
    cpu->regs[VM_REG_A0] = delta > 0 ? (uint32_t)delta : 0;
}

/* ============================================================
 *  Installation
 * ============================================================ */

bool vm_host_install_platform(VmSystem *sys,
                               const VmHostPlatformConfig *cfg) {
    if (!sys || !sys->ecall_router) return false;

    /* Configure module state. */
    if (cfg) {
        g_realtime_source   = cfg->realtime_source;
        g_realtime_userdata = cfg->realtime_userdata;
        if (cfg->rand_seed != 0) g_rand_state = cfg->rand_seed;
    } else {
        g_realtime_source   = NULL;
        g_realtime_userdata = NULL;
    }

    /* Register the syscalls. If any registration fails, roll
     * back the others. */
    if (!vm_ecall_register(sys->ecall_router, SYS_FORMAT_AND_WRITE,
                           handle_format_and_write)) goto fail;
    if (!vm_ecall_register(sys->ecall_router, SYS_FORMAT_TO_BUF,
                           handle_format_to_buf)) goto fail_faw;
    if (!vm_ecall_register(sys->ecall_router, SYS_REALTIME_NOW,
                           handle_realtime_now)) goto fail_ftb;
    if (!vm_ecall_register(sys->ecall_router, SYS_ALLOC_SIZE,
                           handle_alloc_size)) goto fail_rtn;
    if (!vm_ecall_register(sys->ecall_router, SYS_RAND,
                           handle_rand)) goto fail_as;
    if (!vm_ecall_register(sys->ecall_router, SYS_TIMING_DEADLINE_REMAINING,
                           handle_timing_deadline_remaining)) goto fail_rand;
    return true;

fail_rand:    vm_ecall_unregister(sys->ecall_router, SYS_RAND);
fail_as:      vm_ecall_unregister(sys->ecall_router, SYS_ALLOC_SIZE);
fail_rtn:     vm_ecall_unregister(sys->ecall_router, SYS_REALTIME_NOW);
fail_ftb:     vm_ecall_unregister(sys->ecall_router, SYS_FORMAT_TO_BUF);
fail_faw:     vm_ecall_unregister(sys->ecall_router, SYS_FORMAT_AND_WRITE);
fail:         return false;
}
