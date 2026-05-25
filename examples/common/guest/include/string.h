/* ============================================================
 *  string.h — minimal string ops for microgarbage guest VMs
 *
 *  memcpy/memset/memmove/memcmp/strlen/strcmp/strchr go through
 *  libc-accel syscalls. The rest are tiny guest-side impls.
 *
 *  Public domain (CC0).
 * ============================================================ */

#ifndef MICROGARBAGE_GUEST_STRING_H
#define MICROGARBAGE_GUEST_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   *memcpy  (void *dst, const void *src, size_t n);
void   *memset  (void *dst, int c, size_t n);
void   *memmove (void *dst, const void *src, size_t n);
int     memcmp  (const void *a, const void *b, size_t n);

size_t  strlen  (const char *s);
int     strcmp  (const char *a, const char *b);
int     strncmp (const char *a, const char *b, size_t n);
char   *strchr  (const char *s, int c);
char   *strrchr (const char *s, int c);
char   *strstr  (const char *hay, const char *needle);
char   *strcpy  (char *dst, const char *src);
char   *strncpy (char *dst, const char *src, size_t n);
char   *strcat  (char *dst, const char *src);
char   *strncat (char *dst, const char *src, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_GUEST_STRING_H */
