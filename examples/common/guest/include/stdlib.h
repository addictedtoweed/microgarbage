/* ============================================================
 *  stdlib.h — minimal stdlib for microgarbage guest VMs
 *
 *  Backed by host syscalls (SYS_ALLOC, SYS_FREE, SYS_ALLOC_SIZE,
 *  SYS_EXIT, SYS_RAND).
 *
 *  Public domain (CC0).
 * ============================================================ */

#ifndef MICROGARBAGE_GUEST_STDLIB_H
#define MICROGARBAGE_GUEST_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory. */
void *malloc  (size_t size);
void  free    (void *ptr);
void *calloc  (size_t n, size_t sz);
void *realloc (void *ptr, size_t new_size);

/* Exit. */
_Noreturn void exit  (int code);
_Noreturn void abort (void);

/* Random. */
#define RAND_MAX 0x7fffffff
int  rand  (void);
void srand (unsigned seed);

/* Integer math (compiler intrinsics work fine in freestanding;
 * we just declare the standard prototypes). */
int   abs  (int x);
long  labs (long x);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_GUEST_STDLIB_H */
