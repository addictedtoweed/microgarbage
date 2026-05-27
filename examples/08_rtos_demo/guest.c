/* guest.c — a tiny SDK guest for the RTOS demo.
 *
 * Prints a tagged line several times with busy-work between, so the
 * preemptive scheduler visibly time-slices this VM against the others.
 * Built three times with -DGUEST_NAME="A"/"B"/"C" (see build.sh).
 *
 * Uses the shared guest SDK: vm_runtime provides _start (which calls
 * main), and puts() routes to the host over SYS_WRITE.
 *
 * Public domain (CC0). No warranty.
 */
#include "vm_runtime.h"
#include <stdio.h>          /* puts -> SYS_WRITE */

#ifndef GUEST_NAME
#define GUEST_NAME "?"
#endif
#ifndef GUEST_ROUNDS
#define GUEST_ROUNDS 6
#endif

int main(void) {
    for (int i = 0; i < GUEST_ROUNDS; i++) {
        puts("[" GUEST_NAME "] working");
        /* Burn CPU so the 1 ms systick preempts us mid-loop and the
         * scheduler switches to another guest VM. */
        volatile unsigned long x = 0;
        for (unsigned long k = 0; k < 8000000UL; k++) x += k;
    }
    puts("[" GUEST_NAME "] done");
    return 0;
}
