/* platform/rpi/kernel.ld — bare-metal Pi Zero image layout.
 * Loads at 0x8000 (where the GPU starts the ARM). .text.boot (the entry)
 * goes first. .bss is zeroed by boot.S; the stack lives high in RAM. */
ENTRY(_start)

SECTIONS
{
    . = 0x8000;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    }

    _end = .;                 /* heap (_sbrk) grows up from here */
    _stack_top = 0x08000000;  /* 128 MB, grows down (clear of image + FB) */
}
