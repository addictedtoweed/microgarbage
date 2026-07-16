/* platform/rpi/boot.S — bare-metal ARM entry (real kernel.img).
 *
 * Replaces rdimon's semihosting crt0: the GPU loads kernel.img at
 * 0x8000 and starts the ARM here (SVC mode, MMU off, single core on the
 * Pi Zero). We set the stack, zero .bss, and jump to C. No semihosting
 * anywhere — output is the PL011 UART; there is no "exit", so main
 * returning just halts (a real kernel runs forever).
 */
.section ".text.boot"
.global _start
_start:
    ldr   sp, =_stack_top          /* supervisor stack */

    /* zero the .bss (BSS holds the VM pools + the page table) */
    ldr   r0, =_bss_start
    ldr   r1, =_bss_end
    mov   r2, #0
1:  cmp   r0, r1
    bhs   2f
    str   r2, [r0], #4
    b     1b
2:
    bl    main                     /* into C (main.c) */

3:  wfi                            /* main returned: halt the core */
    b     3b
