/* platform/rpi/syscalls.c — minimal newlib syscall stubs (no OS).
 *
 * Replaces rdimon's semihosting versions so the image depends on no
 * host. _write lives in uart.c (routes to the PL011). Here: a bump heap
 * for malloc (_sbrk), a halt for _exit, and inert stubs for the file
 * calls newlib references. These override libnosys.
 *
 * Public domain (CC0). No warranty.
 */

#include <sys/stat.h>
#include <errno.h>

extern char _end;   /* end of .bss (from kernel.ld) — heap starts here */

/* Bump allocator: grows up from _end toward the stack (0x08000000). Only
 * newlib internals malloc here; the VM uses its own static pools. */
void *_sbrk(int incr) {
    static char *heap = 0;
    if (heap == 0) heap = &_end;
    char *prev = heap;
    heap += incr;
    return prev;
}

void _exit(int code) {
    (void)code;
    for (;;) __asm__ volatile("wfi");   /* a kernel doesn't return */
}

int _close(int fd)                     { (void)fd; return -1; }
int _lseek(int fd, int off, int wh)    { (void)fd; (void)off; (void)wh; return 0; }
int _read(int fd, char *buf, int len)  { (void)fd; (void)buf; (void)len; return 0; }
int _isatty(int fd)                    { (void)fd; return 1; }
int _fstat(int fd, struct stat *st)    { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _kill(int pid, int sig)            { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void)                      { return 1; }
