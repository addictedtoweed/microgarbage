/* platform/rpi/guest_hello.c — the RISC-V guest, built with rvcc and
 * embedded into the ARM image. Prints via the SDK mini-libc and does a
 * little compute so you can see RV32 instructions actually executing on
 * the ARM CPU (interpreted).
 *
 * Public domain (CC0). No warranty.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("  hello from the RV32 guest (interpreted on ARM)\n");

    const int n = 10;
    int *f = (int *)malloc((size_t)n * sizeof(int));
    if (!f) { printf("  malloc failed\n"); return 1; }
    f[0] = 0;
    f[1] = 1;
    for (int i = 2; i < n; i++) f[i] = f[i - 1] + f[i - 2];

    printf("  fib:");
    for (int i = 0; i < n; i++) printf(" %d", f[i]);
    printf("\n");

    long sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;
    printf("  sum(1..100) = %ld\n", sum);

    free(f);
    return 0;
}
