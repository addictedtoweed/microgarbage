/* 11_run/hello.c — a small libc guest, built with rvcc.
 *
 * Uses the SDK mini-libc (printf, malloc/free) via the standard runtime
 * (_start -> main). Build it with the dev-kit compiler and run it:
 *
 *     ../../tools/rvcc -o build/hello.elf hello.c
 *     ./build/run build/hello.elf
 *
 * Public domain (CC0). No warranty.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("hello from rvcc + run!\n");
    printf("  %d + %d = %d\n", 2, 40, 42);

    const int n = 8;
    int *fib = (int *)malloc((size_t)n * sizeof(int));
    if (!fib) { printf("  malloc failed\n"); return 1; }
    fib[0] = 0;
    fib[1] = 1;
    for (int i = 2; i < n; i++) fib[i] = fib[i - 1] + fib[i - 2];

    printf("  fib:");
    for (int i = 0; i < n; i++) printf(" %d", fib[i]);
    printf("\n");

    free(fib);
    return 0;
}
