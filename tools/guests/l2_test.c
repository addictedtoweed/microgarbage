/* ============================================================
 *  l2_test.c — guest ELF that exercises alloc_l2 / free_l2.
 *
 *  Allocates three L2 chunks of distinct sizes, writes distinct
 *  patterns into each, reads them back, verifies no trampling,
 *  frees everything, and exits with code 0 on success / a small
 *  positive code on each kind of failure (so the host test can
 *  report which step failed without needing TTY output).
 *
 *  Exit codes:
 *      0  all checks passed
 *      1  alloc_l2 returned NULL
 *      2  pointer in unexpected range (not 0xE000_0000+)
 *      3  pattern read-back mismatch (overlap or bad translation)
 *      4  free_l2 + realloc didn't reuse the freed space
 *
 *  Built against examples/common/guest/{vm_runtime,mg_l2}.h.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_l2.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

static void die(int code) { sys_exit(code); }

static int check_va(void *p) {
    if (!p) return 0;
    /* L2 VAs live in 0xE000_0000..0xFFFF_FFFF. */
    return ((uintptr_t)p & 0xE0000000u) == 0xE0000000u;
}

void _start(void) {
    /* 1. Three distinct allocations. */
    void *a = alloc_l2(1024,   0);    /* 1 KB  */
    void *b = alloc_l2(64*1024, 0);   /* 64 KB */
    void *c = alloc_l2(16,     0);    /* tiny  */

    if (!a || !b || !c) die(1);
    if (!check_va(a) || !check_va(b) || !check_va(c)) die(2);

    /* 2. Write three distinct patterns. */
    uint8_t *pa = (uint8_t *)a;
    uint8_t *pb = (uint8_t *)b;
    uint8_t *pc = (uint8_t *)c;
    for (int i = 0; i < 1024; i++)      pa[i] = (uint8_t)(0xAA + (i & 1));
    for (int i = 0; i < 64*1024; i++)   pb[i] = (uint8_t)(0x55 + (i & 3));
    for (int i = 0; i < 16; i++)        pc[i] = (uint8_t)(0xCC ^ i);

    /* 3. Read back. */
    for (int i = 0; i < 1024; i++)      if (pa[i] != (uint8_t)(0xAA + (i & 1))) die(3);
    for (int i = 0; i < 64*1024; i++)   if (pb[i] != (uint8_t)(0x55 + (i & 3))) die(3);
    for (int i = 0; i < 16; i++)        if (pc[i] != (uint8_t)(0xCC ^ i))       die(3);

    /* 4. Free middle, realloc same size — verify the freed space
     *    is reused. Check by comparing the new pointer's offset
     *    against b's offset: they should be the same. */
    uintptr_t b_va = (uintptr_t)b;
    free_l2(b);
    void *b2 = alloc_l2(64*1024, 0);
    if (!b2 || !check_va(b2)) die(4);
    if ((uintptr_t)b2 != b_va) die(4);

    /* 5. Verify b2 reads write/read cleanly. */
    uint8_t *pb2 = (uint8_t *)b2;
    for (int i = 0; i < 64*1024; i++)   pb2[i] = (uint8_t)(0x11 ^ i);
    for (int i = 0; i < 64*1024; i++)   if (pb2[i] != (uint8_t)(0x11 ^ i)) die(3);

    /* 6. Cleanup. */
    free_l2(a);
    free_l2(b2);
    free_l2(c);

    sys_exit(0);
}
