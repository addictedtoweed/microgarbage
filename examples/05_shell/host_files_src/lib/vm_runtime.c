/* ============================================================
 *  vm_runtime.c — guest-side crt0 + libc bridge
 *
 *  Provides:
 *    - _start: zero BSS, set up stdio FILE objects, call main(),
 *              pass return value to SYS_EXIT
 *    - malloc/free/calloc/realloc over SYS_ALLOC/SYS_FREE/
 *              SYS_ALLOC_SIZE
 *    - printf/snprintf/vprintf/vsnprintf/fprintf/puts/
 *              putchar/fputs/fputc/fflush over SYS_FORMAT_*
 *    - memcpy/memset/memmove/memcmp/strlen/strcmp/strncmp/
 *              strchr/strrchr/strstr/strncpy/strcat over the
 *              libc-accel syscalls and small inline impls
 *    - exit/abort over SYS_EXIT
 *    - time over SYS_REALTIME_NOW; clock over SYS_TICKS_NOW
 *    - rand/srand over SYS_RAND (srand is a no-op; host owns the seed)
 *
 *  This file gets compiled into every guest that #includes any
 *  of the standard headers under lib/include/. The build script
 *  links it unconditionally — guests that use NONE of libc still
 *  get _start, which is harmless.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm_runtime.h"

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The compiler tries to call memset/memcpy from generated code
 * (e.g., struct copies, large-zero inits). We provide them with
 * external linkage so the linker resolves to our version. */

/* ============================================================
 *  Linker symbols — BSS bounds.
 * ============================================================ */
extern uint8_t __bss_start;
extern uint8_t __bss_end;

/* ============================================================
 *  FILE — minimal stdio file objects
 *
 *  Just an integer fd inside a struct, so user code can hold a
 *  FILE *. We provide stdin, stdout, stderr as globals pointing
 *  at three statically-allocated instances. fopen/fclose for
 *  files are NOT here — guests that want them should use
 *  SYS_OPENAT directly (or a future T.x extension).
 * ============================================================ */

typedef struct __vm_FILE {
    int fd;
} FILE;       /* This is the type, but the public typedef lives in <stdio.h>. */

/* The standard names. Defined as proper objects (not pointers)
 * so &stdin works the same way as in real libc. We expose them
 * to user code as FILE * via <stdio.h>. */
static struct __vm_FILE _vm_stdin  = { .fd = 0 };
static struct __vm_FILE _vm_stdout = { .fd = 1 };
static struct __vm_FILE _vm_stderr = { .fd = 2 };

/* The public symbols user code references via <stdio.h>:
 *   #define stdin   (&__vm_stdin)
 * So we need to expose the underlying objects. */
struct __vm_FILE *const __vm_stdin  = &_vm_stdin;
struct __vm_FILE *const __vm_stdout = &_vm_stdout;
struct __vm_FILE *const __vm_stderr = &_vm_stderr;

/* ============================================================
 *  Memory functions.
 *
 *  Implemented inline in the guest — these are tiny enough that
 *  the syscall trampoline costs more than the work. (The host
 *  exposes SYS_MEMCPY etc. with the right numbers in vm_ecall.h
 *  but those handlers aren't registered yet; even if they were,
 *  for n < ~128 the syscall overhead dominates.)
 * ============================================================ */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    while (n--) *d++ = v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/* ============================================================
 *  String functions.
 *
 *  All guest-side. Same rationale as the mem ops above.
 * ============================================================ */

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    for (;; s++) {
        if ((unsigned char)*s == ch) return (char *)s;
        if (*s == 0) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    unsigned char ch = (unsigned char)c;
    for (; *s; s++) if ((unsigned char)*s == ch) last = s;
    if (ch == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        if (strncmp(hay, needle, nl) == 0) return (char *)hay;
    }
    return NULL;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    for (size_t i = 0; i < n && src[i]; i++) *d++ = src[i];
    *d = '\0';
    return dst;
}

/* ============================================================
 *  Memory allocator (libc-shaped over SYS_ALLOC/FREE/ALLOC_SIZE)
 *
 *  Guest pointers are 32-bit; uintptr_t is the same width. The
 *  shared slab in the host gives us bucket-rounded blocks. We
 *  query the rounded size when realloc needs to bound a copy.
 * ============================================================ */

void *malloc(size_t size) {
    if (size == 0) return NULL;
    int32_t r = (int32_t)_vm_sys1(SYS_ALLOC, (uint32_t)size);
    if (r < 0 && r > -4096) return NULL;     /* error range */
    return (void *)(uintptr_t)(uint32_t)r;
}

void free(void *ptr) {
    if (!ptr) return;
    _vm_sys1(SYS_FREE, (uint32_t)(uintptr_t)ptr);
}

void *calloc(size_t n, size_t sz) {
    /* Overflow check. */
    if (n != 0 && sz > (SIZE_MAX / n)) return NULL;
    size_t total = n * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }
    /* Honest realloc: copy at most min(old_block, new_size). */
    int32_t old = (int32_t)_vm_sys1(SYS_ALLOC_SIZE,
                                     (uint32_t)(uintptr_t)ptr);
    if (old < 0) {
        /* The pointer isn't from our allocator; treat like
         * undefined behavior — but be conservative: alloc new,
         * don't free old, return new (caller probably crashes
         * on the next access). */
        return malloc(new_size);
    }
    void *np = malloc(new_size);
    if (!np) return NULL;
    size_t copy = (size_t)old < new_size ? (size_t)old : new_size;
    memcpy(np, ptr, copy);
    free(ptr);
    return np;
}

/* ============================================================
 *  Exit / abort
 * ============================================================ */

_Noreturn void exit(int code) {
    _vm_sys1(SYS_EXIT, (uint32_t)code);
    /* SYS_EXIT doesn't return, but the compiler can't prove that. */
    for (;;) { }
}

_Noreturn void abort(void) {
    exit(127);
}

/* ============================================================
 *  stdio output — printf, snprintf, etc.
 *
 *  All formatting happens in the host (SYS_FORMAT_*). We pack
 *  the variadic args into a u32 array and pass that across.
 *
 *  Up to 16 args supported. Args wider than 32 bits would need
 *  a different ABI; we don't support %lld / %f / %g (consistent
 *  with the host formatter's limitations).
 * ============================================================ */

#define VM_FMT_MAX_ARGS 16

/* Walk the format string once to find how many args to pull.
 * Returns the count to pull from the va_list. Each conversion
 * specifier consumes one slot (we don't support % * dynamic
 * width/precision in this initial release; that'd add 1-2 more
 * pulls per spec). */
static unsigned count_format_args(const char *fmt) {
    unsigned n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%' || *p == 0) continue;
        /* Skip flags, width, precision — but NOT the conversion. */
        while (*p == '-' || *p == '+' || *p == ' ' ||
               *p == '#' || *p == '0') p++;
        if (*p == '*') { n++; p++; }            /* width = *  */
        else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            if (*p == '*') { n++; p++; }        /* precision = *  */
            else while (*p >= '0' && *p <= '9') p++;
        }
        /* Length modifiers (ignored). */
        while (*p == 'h' || *p == 'l' || *p == 'z' ||
               *p == 't' || *p == 'j') p++;
        if (!*p) break;
        /* Conversion specifier consumes one arg. */
        if (*p == 's' || *p == 'd' || *p == 'i' || *p == 'u' ||
            *p == 'x' || *p == 'X' || *p == 'o' || *p == 'c' ||
            *p == 'p') n++;
        if (n >= VM_FMT_MAX_ARGS) return VM_FMT_MAX_ARGS;
    }
    return n;
}

/* Pack `n` args from `ap` into `args[]` as u32. */
static void pack_args(va_list ap, uint32_t *args, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        /* On RV32 everything is 32-bit anyway. We pull as
         * unsigned int to avoid sign-extension surprises. */
        args[i] = (uint32_t)va_arg(ap, unsigned int);
    }
}

int vprintf(const char *fmt, va_list ap) {
    unsigned n = count_format_args(fmt);
    uint32_t args[VM_FMT_MAX_ARGS];
    pack_args(ap, args, n);
    return (int)_vm_sys4(SYS_FORMAT_AND_WRITE, 1,
                         (uint32_t)(uintptr_t)fmt,
                         (uint32_t)(uintptr_t)args, n);
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    unsigned n = count_format_args(fmt);
    uint32_t args[VM_FMT_MAX_ARGS];
    pack_args(ap, args, n);
    return (int)_vm_sys4(SYS_FORMAT_AND_WRITE, (uint32_t)f->fd,
                         (uint32_t)(uintptr_t)fmt,
                         (uint32_t)(uintptr_t)args, n);
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    unsigned n = count_format_args(fmt);
    uint32_t args[VM_FMT_MAX_ARGS];
    pack_args(ap, args, n);
    return (int)_vm_sys5(SYS_FORMAT_TO_BUF,
                         (uint32_t)(uintptr_t)buf, (uint32_t)cap,
                         (uint32_t)(uintptr_t)fmt,
                         (uint32_t)(uintptr_t)args, n);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    /* puts adds a trailing newline. Two SYS_WRITEs: cheaper than
     * a SYS_FORMAT round-trip for a literal string. */
    size_t n = strlen(s);
    int32_t r1 = (int32_t)_vm_sys3(SYS_WRITE, 1,
                                    (uint32_t)(uintptr_t)s, (uint32_t)n);
    if (r1 < 0) return -1;
    char nl = '\n';
    int32_t r2 = (int32_t)_vm_sys3(SYS_WRITE, 1,
                                    (uint32_t)(uintptr_t)&nl, 1);
    if (r2 < 0) return -1;
    return (int)(r1 + r2);
}

int putchar(int c) {
    char ch = (char)c;
    int32_t r = (int32_t)_vm_sys3(SYS_WRITE, 1,
                                   (uint32_t)(uintptr_t)&ch, 1);
    return r < 0 ? -1 : (unsigned char)c;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    int32_t r = (int32_t)_vm_sys3(SYS_WRITE, (uint32_t)f->fd,
                                   (uint32_t)(uintptr_t)s, (uint32_t)n);
    return r < 0 ? -1 : (int)r;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    int32_t r = (int32_t)_vm_sys3(SYS_WRITE, (uint32_t)f->fd,
                                   (uint32_t)(uintptr_t)&ch, 1);
    return r < 0 ? -1 : (unsigned char)c;
}

int fflush(FILE *f) {
    /* Flush is best-effort — there's no buffer in our FILE. */
    if (!f) return 0;
    _vm_sys1(SYS_FFLUSH, (uint32_t)f->fd);
    return 0;
}

/* ============================================================
 *  stdio input — fgetc, getchar.
 *  We don't implement fgets/scanf yet; they need a guest-side
 *  line buffer that the FILE object doesn't carry.
 * ============================================================ */

int fgetc(FILE *f) {
    char c;
    int32_t r = (int32_t)_vm_sys3(SYS_READ, (uint32_t)f->fd,
                                   (uint32_t)(uintptr_t)&c, 1);
    if (r <= 0) return -1;       /* EOF or error */
    return (unsigned char)c;
}

int getchar(void) {
    return fgetc(&_vm_stdin);
}

/* ============================================================
 *  Time
 * ============================================================ */

/* time_t is whatever the host says. We assume 32-bit Unix epoch
 * seconds (signed). */
typedef int32_t time_t;

typedef struct {
    uint32_t version;
    uint32_t seconds;
    uint32_t nanos;
} __vm_realtime_record;

time_t time(time_t *out) {
    __vm_realtime_record rec;
    int32_t r = (int32_t)_vm_sys1(SYS_REALTIME_NOW,
                                   (uint32_t)(uintptr_t)&rec);
    if (r < 0) {
        if (out) *out = -1;
        return -1;
    }
    if (out) *out = (time_t)rec.seconds;
    return (time_t)rec.seconds;
}

/* clock() returns CPU time in "clocks". For us, it's monotonic
 * milliseconds-since-host-start. Define CLOCKS_PER_SEC = 1000
 * in <time.h>. */
typedef int32_t clock_t;

clock_t clock(void) {
    return (clock_t)_vm_sys0(SYS_TICKS_NOW);
}

/* ============================================================
 *  Random
 * ============================================================ */

int rand(void) {
    /* RAND_MAX is INT32_MAX in our headers, so we mask off the
     * high bit. */
    return (int)(_vm_sys0(SYS_RAND) & 0x7fffffffu);
}

void srand(unsigned seed) {
    /* No-op: the host owns the PRNG state. We could expose
     * SYS_SET_RAND_SEED later if guests want deterministic
     * sequences in tests. */
    (void)seed;
}

/* ============================================================
 *  _start — guest entry.
 *
 *  This is what the linker sets as the program entry point. It:
 *    1. Zeroes BSS (the linker script may also do this; doing
 *       it here is the belt-and-braces approach)
 *    2. Calls main()
 *    3. Passes main's return value to SYS_EXIT
 *
 *  The guest writes `int main(void)` (or `int main(int argc,
 *  char **argv)` — we pass argc=0, argv=NULL). _start is never
 *  visible to user code.
 * ============================================================ */

extern int main(int argc, char **argv);

_Noreturn __attribute__((weak)) void _start(void) {
    /* Zero BSS. */
    uint8_t *p = &__bss_start;
    uint8_t *end = &__bss_end;
    while (p < end) *p++ = 0;

    /* Call main. */
    int ret = main(0, (char **)0);

    /* Exit with main's return value. */
    exit(ret);
}
