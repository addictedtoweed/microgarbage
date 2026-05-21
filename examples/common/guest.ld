/* Common linker script for VM guest programs.
 *
 * The VM has four address regions (each 1 GB, addressed by the
 * top 2 bits of the 32-bit address):
 *
 *   region 0  CODE     0x00000000 - 0x3FFFFFFF   R-X
 *   region 1  RODATA   0x40000000 - 0x7FFFFFFF   R--
 *   region 2  DATA     0x80000000 - 0xBFFFFFFF   RW-   (stack lives here too)
 *   region 3  SHARED   0xC0000000 - 0xFFFFFFFF   RW-   (allocated via SYS_ALLOC)
 *
 * The loader allocates each region's actual storage at load time;
 * what matters for the linker is the VIRTUAL address layout. The
 * loader maps .text to region 0 base, .rodata to region 1 base,
 * .data and .bss to region 2 base.
 *
 * Guests don't have a heap; they use SYS_ALLOC for dynamic memory.
 * The stack lives at the top of region 2 and grows down; the
 * loader sets SP at load time. Guests don't need to manage it. */

ENTRY(_start)

MEMORY {
    code   (rx) : ORIGIN = 0x00000000, LENGTH = 0x10000   /* 64 KB */
    rodata (r)  : ORIGIN = 0x40000000, LENGTH = 0x10000
    data   (rw) : ORIGIN = 0x80000000, LENGTH = 0x10000
}

SECTIONS {
    .text   : { *(.text*) }   > code
    .rodata : { *(.rodata*) } > rodata
    .data   : { *(.data*) }   > data
    .bss    : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    } > data
}
