/* libcdemo.c — proof-of-concept guest using only standard C.
 *
 * No #include of any VM-specific headers. No _start. No syscall
 * wrappers. Just plain C.
 *
 * Demonstrates that the lib/include/ headers + vm_runtime.c
 * bridge let you write portable-looking code that runs on the
 * microgarbage VM.
 *
 * Public domain (CC0).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* 1. printf with format specifiers. */
    printf("libcdemo: hello from a standard-C guest\n");
    printf("libcdemo: int=%d unsigned=%u hex=0x%08x\n",
           -42, 12345u, 0xdeadbeef);

    /* 2. snprintf to a stack buffer. */
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "score: %d/%d (%d%%)", 17, 25, 68);
    printf("libcdemo: snprintf returned %d, buf='%s'\n", n, buf);

    /* 3. Dynamic memory. */
    char *heap = malloc(128);
    if (!heap) {
        fprintf(stderr, "libcdemo: malloc failed\n");
        return 1;
    }
    strcpy(heap, "this lives on the heap");
    printf("libcdemo: heap='%s' (len=%u)\n", heap, (unsigned)strlen(heap));

    /* 4. Realloc — grow the block, verify the original contents
     * survive the copy. */
    char *bigger = realloc(heap, 256);
    if (!bigger) {
        fprintf(stderr, "libcdemo: realloc failed\n");
        free(heap);
        return 1;
    }
    heap = bigger;
    strcat(heap, "; and now there's more room");
    printf("libcdemo: after realloc + strcat: '%s'\n", heap);

    /* 5. memset / memcmp. */
    char a[16], b[16];
    memset(a, 0x55, sizeof(a));
    memset(b, 0x55, sizeof(b));
    printf("libcdemo: memcmp(a, b) = %d (expect 0)\n", memcmp(a, b, sizeof(a)));
    b[7] = 0x56;
    printf("libcdemo: after b[7]=0x56, memcmp = %d (expect <0)\n",
           memcmp(a, b, sizeof(a)));

    /* 6. Some string ops. */
    const char *needle = strstr("the quick brown fox", "brown");
    printf("libcdemo: strstr found 'brown' at offset %d\n",
           needle ? (int)(needle - "the quick brown fox") : -1);

    /* 7. Random. */
    printf("libcdemo: rand() three times: %d %d %d\n",
           rand() & 0xff, rand() & 0xff, rand() & 0xff);

    free(heap);
    printf("libcdemo: done\n");
    return 0;
}
