/* ============================================================
 *  stdio.h — minimal stdio for microgarbage guest VMs
 *
 *  Functions backed by the host's platform syscalls
 *  (SYS_FORMAT_AND_WRITE, SYS_WRITE, SYS_READ).
 *
 *  What's here:
 *    printf, vprintf, fprintf, vfprintf
 *    snprintf, vsnprintf
 *    puts, putchar, fputs, fputc, fgetc, getchar
 *    fflush
 *
 *  What's NOT here:
 *    fopen, fclose, fread, fwrite, fseek, ftell, ferror,
 *    fgets, scanf, sscanf, anything involving FILE*
 *      backed by a real file.
 *    (Guests that want file IO call SYS_OPENAT etc. directly.
 *    A future round may add a real FILE* layer.)
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_GUEST_STDIO_H
#define MICROGARBAGE_GUEST_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FILE: opaque-ish. Just an fd holder. Defined in vm_runtime.c. */
typedef struct __vm_FILE FILE;

/* The three standard streams. These are pointers because user
 * code passes them by value to fprintf(stdout, ...) etc. */
extern struct __vm_FILE *const __vm_stdin;
extern struct __vm_FILE *const __vm_stdout;
extern struct __vm_FILE *const __vm_stderr;

#define stdin   __vm_stdin
#define stdout  __vm_stdout
#define stderr  __vm_stderr

/* Standard return value for end-of-file or error. */
#define EOF (-1)

/* Formatted output. */
int  printf  (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  fprintf (FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  snprintf(char *buf, size_t cap, const char *fmt, ...)
              __attribute__((format(printf, 3, 4)));
int  vprintf (const char *fmt, va_list ap);
int  vfprintf(FILE *f, const char *fmt, va_list ap);
int  vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

/* Character output. */
int  puts    (const char *s);
int  putchar (int c);
int  fputs   (const char *s, FILE *f);
int  fputc   (int c, FILE *f);
int  fflush  (FILE *f);

/* Character input. */
int  fgetc   (FILE *f);
int  getchar (void);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_GUEST_STDIO_H */
