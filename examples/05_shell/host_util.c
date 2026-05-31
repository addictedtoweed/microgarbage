/* ============================================================
 *  host_util.c — implementation of the host_util.h leaf helpers.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* mkdir is single-arg on mingw (Windows has no Unix mode concept). */
int host_mkdir(const char *path, int mode) {
#if defined(_WIN32)
    (void)mode;
    return mkdir(path);
#else
    return mkdir(path, (mode_t)mode);
#endif
}

bool host_exe_dir(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';

#if defined(_WIN32)
    char path[1024];
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) return false;
    char *sep = NULL;
    for (char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') sep = p;
    if (!sep) return false;
    *sep = '\0';
    if (strlen(path) + 1 > out_sz) return false;
    strcpy(out, path);
    return true;
#elif defined(__linux__)
    char path[1024];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0 || (size_t)n >= sizeof(path)) return false;
    path[n] = '\0';
    char *sep = strrchr(path, '/');
    if (!sep) return false;
    *sep = '\0';
    if (strlen(path) + 1 > out_sz) return false;
    strcpy(out, path);
    return true;
#else
    /* Cygwin / other POSIX: no portable self-exe path used here.
     * Fall back to CWD-relative (previous behaviour). */
    (void)out_sz;
    return false;
#endif
}

void warn_if_no_real_console(void) {
#if defined(_WIN32)
    /* Native Windows: stdin not being a real console means we were
     * launched under mintty / a Cygwin pty. There Ctrl-C is NOT
     * delivered as CTRL_C_EVENT (mintty is a pty, not a console) and
     * the native process doesn't see Cygwin's POSIX SIGINT either, so
     * Ctrl-C appears dead once a session is connected. Tell the user
     * how to get working Ctrl-C. GetConsoleMode on the stdin handle
     * succeeds only for a genuine console. */
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) {
        fprintf(stderr,
            "host: NOTE - no real Windows console detected (looks like\n"
            "      mintty / a Cygwin pty). Ctrl-C may not stop the host\n"
            "      once a session is connected. For working Ctrl-C, run\n"
            "      host.exe from cmd.exe or PowerShell, or under mintty\n"
            "      use 'winpty ./host.exe ...', or stop it with\n"
            "      'kill -INT <pid>' from another terminal.\n");
        fflush(stderr);
    }
#else
    /* Non-native-Windows builds always have a usable controlling
     * terminal for our purposes; nothing to warn about. */
#endif
}

int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
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
