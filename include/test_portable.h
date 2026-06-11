/* ============================================================
 *  test_portable.h — tiny portability shims for the test suites
 *
 *  A few host-shim tests capture stdout / feed stdin / create temp
 *  files, which means POSIX calls that native Windows (mingw-w64)
 *  spells differently or lacks. These wrappers let those suites build
 *  and run on native Windows as well as Cygwin/Linux:
 *
 *    tp_tmpdir()      writable temp directory (no trailing separator)
 *    tp_path(b,c,n)   "<tmpdir>/<n>" composed into b
 *    tp_mkdir(p)      mkdir (1-arg on Windows, mode 0755 on POSIX)
 *    tp_fsync(fd)     flush a file descriptor (_commit / fsync)
 *    tp_pipe(fds)     create a pipe (_pipe / pipe)
 *
 *  Test-only; not part of the shipped library.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef TEST_PORTABLE_H
#define TEST_PORTABLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Temp directory from the environment, platform fallback otherwise.
 * The test harness (run_tests.sh) sets TMP/TMPDIR. */
static inline const char *tp_tmpdir(void) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = getenv("TEMP");
    if (!t || !*t) t = getenv("TMP");
#if defined(_WIN32)
    if (!t || !*t) t = ".";
#else
    if (!t || !*t) t = "/tmp";
#endif
    return t;
}

/* Compose "<tmpdir>/<name>" into buf (caller-sized); returns buf. */
static inline char *tp_path(char *buf, size_t cap, const char *name) {
    snprintf(buf, cap, "%s/%s", tp_tmpdir(), name);
    return buf;
}

#if defined(_WIN32)
  #include <io.h>      /* _commit, _pipe */
  #include <direct.h>  /* _mkdir */
  #include <fcntl.h>   /* _O_BINARY */
  static inline int tp_mkdir(const char *p) { return _mkdir(p); }
  static inline int tp_fsync(int fd)        { return _commit(fd); }
  static inline int tp_pipe(int fds[2])     { return _pipe(fds, 65536, _O_BINARY); }
#else
  #include <unistd.h>
  #include <sys/stat.h>
  static inline int tp_mkdir(const char *p) { return mkdir(p, 0755); }
  static inline int tp_fsync(int fd)        { return fsync(fd); }
  static inline int tp_pipe(int fds[2])     { return pipe(fds); }
#endif

#endif /* TEST_PORTABLE_H */
