/* platform/rpi/guest_hello.c — the RISC-V guest, built with rvcc and
 * embedded into the ARM image. Prints via the SDK mini-libc and does a
 * little compute so you can see RV32 instructions actually executing on
 * the ARM CPU (interpreted).
 *
 * Public domain (CC0). No warranty.
 */

#include <stdio.h>
#include <stdlib.h>
#include "hwio.h"   /* portable GPIO/I2C/... — same header as the desktop sim */

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

    /* Drive a real BCM2835 GPIO pin, through the portable hwio ecalls:
     * guest -> SYS_GPIO_* -> vm_host_hwio -> platform_rpi.c -> registers.
     * The identical calls run against the desktop sim in examples/10. */
    hwio_gpio_config(17, HWIO_GPIO_OUT);
    hwio_gpio_write(17, 1);
    printf("  gpio17: write 1 -> reads %d\n", hwio_gpio_read(17));
    hwio_gpio_toggle(17);
    printf("  gpio17: toggle  -> reads %d\n", hwio_gpio_read(17));

    free(f);
    return 0;
}
