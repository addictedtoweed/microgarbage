/* ============================================================
 *  vm_loader.h — RV32 ELF loader for the VM
 *
 *  Loads a statically-linked RV32 ELF image into a VmCpu's
 *  region descriptors. Honors the VM's top-2-bits region scheme:
 *  segments whose virtual addresses fall in 0x0000_0000-0x3FFF_FFFF
 *  go to region 0 (CODE), 0x4000_0000-0x7FFF_FFFF to region 1
 *  (RODATA), 0x8000_0000-0xBFFF_FFFF to region 2 (DATA).
 *
 *  Region 3 (SHARED) is not populated from the ELF — it is system-
 *  wide state passed in by the caller, identical for every VM.
 *
 *  ---------------------------------------------------------------
 *  Backing modes
 *  ---------------------------------------------------------------
 *
 *  Code (region 0) and rodata (region 1) can be backed two ways:
 *
 *    VM_BACKING_XIP        — point the region at the ELF image
 *                            in place. No copy. The ELF image
 *                            must remain resident at the same
 *                            address for the lifetime of the VM.
 *                            Saves RAM but pays a flash-access
 *                            penalty on every fetch from these
 *                            regions.
 *
 *    VM_BACKING_COPY_RAM   — allocate RAM from the caller's bump
 *                            arena and copy the segment bytes
 *                            into it. The ELF image may be
 *                            unmapped after vm_load returns.
 *                            Faster execution from these regions
 *                            at the cost of RAM.
 *
 *  Data (region 2) is always backed by RAM. The loader allocates
 *  region_data_size bytes from the bump arena, copies the data
 *  segment's file bytes (initialized data) to the bottom, zeroes
 *  the next chunk (BSS), and leaves the remainder for the guest's
 *  stack (growing down from the top) and any guest-side heap.
 *
 *  Shared (region 3) is set up by the caller, not the loader.
 *  Pass the host pointer and size in the config; the loader
 *  copies them into the region descriptor.
 *
 *  ---------------------------------------------------------------
 *  Stack pointer initialization
 *  ---------------------------------------------------------------
 *
 *  The loader initializes sp (x2) to point at the top of region 2,
 *  8-byte aligned, growing down. Specifically:
 *
 *      sp = 0x80000000 + region_data_size,  rounded down to 8-byte alignment
 *
 *  This is a convention between the loader and the guest's crt0;
 *  guest startup code expects sp to be set on entry and does not
 *  set it itself. Standard riscv32-unknown-elf crt0 with no
 *  custom startup code matches this convention.
 *
 *  The data segment (initialized + BSS) lives at the bottom of
 *  region 2. The stack grows down from the top. The guest's heap
 *  (if any — guest-side malloc) lives in between, growing up
 *  toward the stack. The loader does not enforce any partition;
 *  the guest is responsible for not letting stack and heap
 *  collide.
 *
 *  ---------------------------------------------------------------
 *  Strict validation
 *  ---------------------------------------------------------------
 *
 *  The loader rejects any ELF feature it doesn't fully understand,
 *  reporting a specific error code so the failure is debuggable.
 *  Rejections include:
 *
 *    - Wrong magic, class, endianness, version
 *    - Machine type not EM_RISCV (0xF3)
 *    - Type not ET_EXEC (no relocatable, no shared object)
 *    - PT_DYNAMIC present (dynamic linking forbidden)
 *    - PT_INTERP present (dynamic interpreter forbidden)
 *    - PT_LOAD segment outside the three populated regions
 *    - PT_LOAD writable bit set in regions 0 or 1
 *    - PT_LOAD memsz > filesz on regions 0 or 1 (suggests BSS in
 *      code/rodata, which makes no sense)
 *    - Entry point not in region 0
 *    - Entry point not 2-byte aligned (C-extension minimum)
 *    - Unresolved relocations (any .rela.* section with content)
 *
 *  PT_NOTE, PT_PHDR, PT_GNU_STACK, PT_GNU_RELRO and other GNU
 *  extension headers are accepted and ignored — they're harmless
 *  metadata that lld emits by default.
 *
 *  ---------------------------------------------------------------
 *  Linker script expectations
 *  ---------------------------------------------------------------
 *
 *  Guests are built with a linker script that places sections at
 *  the VM's region addresses. A minimal viable script:
 *
 *      MEMORY {
 *          CODE   (rx) : ORIGIN = 0x00000000, LENGTH = 16M
 *          RODATA (r ) : ORIGIN = 0x40000000, LENGTH = 16M
 *          DATA   (rw) : ORIGIN = 0x80000000, LENGTH = 16M
 *      }
 *      SECTIONS {
 *          .text   : { *(.text*)   } > CODE
 *          .rodata : { *(.rodata*) } > RODATA
 *          .data   : { *(.data*)   } > DATA
 *          .bss    : { *(.bss*) *(COMMON) } > DATA
 *      }
 *      ENTRY(_start)
 *
 *  The LENGTH values are just the guest's expectations for layout
 *  purposes; the actual region sizes at runtime are determined by
 *  the loader (filesz for XIP, region_data_size for DATA). LENGTH
 *  must be large enough to hold the sections, small enough to fit
 *  in 1 GB per region.
 *
 *  ---------------------------------------------------------------
 *  Depends on: vm_core, memory/bump
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_LOADER_H
#define VM_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vm/vm_core.h"
#include "memory/slab_stack.h"

/* ============================================================
 *  Backing modes (per-region choice for code and rodata)
 * ============================================================ */

typedef enum {
    /* Point the region descriptor at the ELF image in place.
     * The ELF image must remain at the same host address for the
     * VM's lifetime. */
    VM_BACKING_XIP = 0,

    /* Allocate from the bump arena and copy the segment in. The
     * ELF image is no longer referenced after vm_load returns. */
    VM_BACKING_COPY_RAM,
} VmBacking;

/* ============================================================
 *  Result codes
 *
 *  Specific enough that a failure tells you which ELF feature
 *  caused it — easier to debug than a single "ELF invalid" code.
 * ============================================================ */

typedef enum {
    VM_LOAD_OK = 0,

    /* Header-level rejections */
    VM_LOAD_ERR_TRUNCATED,           /* image smaller than ELF header */
    VM_LOAD_ERR_BAD_MAGIC,           /* not "\x7fELF" */
    VM_LOAD_ERR_NOT_RV32,            /* class != ELFCLASS32 */
    VM_LOAD_ERR_NOT_LITTLE_ENDIAN,
    VM_LOAD_ERR_BAD_VERSION,
    VM_LOAD_ERR_NOT_RISCV,           /* e_machine != EM_RISCV (0xF3) */
    VM_LOAD_ERR_NOT_EXECUTABLE,      /* e_type != ET_EXEC */
    VM_LOAD_ERR_BAD_ENTRY,           /* entry not in region 0 or misaligned */

    /* Segment-level rejections */
    VM_LOAD_ERR_DYNAMIC_FORBIDDEN,   /* PT_DYNAMIC present */
    VM_LOAD_ERR_INTERP_FORBIDDEN,    /* PT_INTERP present */
    VM_LOAD_ERR_SEG_OUT_OF_RANGE,    /* PT_LOAD vaddr not in 0,1,2 region */
    VM_LOAD_ERR_SEG_REGION_MISMATCH, /* writable flag wrong for region */
    VM_LOAD_ERR_SEG_BSS_IN_RO,       /* memsz > filesz in code/rodata */
    VM_LOAD_ERR_SEG_TOO_LARGE,       /* segment exceeds 1 GB region cap */

    /* Relocation rejections */
    VM_LOAD_ERR_UNRESOLVED_RELOCS,   /* .rela.* with content present */

    /* Resource exhaustion */
    VM_LOAD_ERR_OUT_OF_RAM,          /* bump arena couldn't satisfy a region */
    VM_LOAD_ERR_DATA_SIZE_TOO_SMALL, /* config region_data_size < data+bss */

    /* Sanity / config errors */
    VM_LOAD_ERR_INVALID_ARG,         /* NULL pointer, etc. */
} VmLoadResult;

/* ============================================================
 *  Loader configuration
 *
 *  Caller fills this in and passes it to vm_load alongside the
 *  ELF image and CPU.
 * ============================================================ */

typedef struct {
    /* --- Per-region backing choice --- */
    VmBacking code_backing;     /* region 0: XIP or COPY_RAM    */
    VmBacking rodata_backing;   /* region 1: XIP or COPY_RAM    */
    /* region 2 (DATA) is always backed by RAM from the arena    */
    /* region 3 (SHARED) is provided directly via the fields below */

    /* --- RAM source for COPY_RAM and the DATA region ---
     *
     * Allocations come from this slab. Per-allocation freeing
     * means vm_unload can fully reclaim a VM's bytes without
     * fragmenting the arena. The slab must have bins large
     * enough to hold:
     *
     *   - The text/rodata block(s) when COPY_RAM mode is in use.
     *     For typical guests these are 4-16 KB; round up to a
     *     reasonable bin size in the slab config.
     *   - The DATA region (region_data_size bytes).
     *
     * If both backings are XIP, the slab is only used for DATA. */
    SlabAllocator *ram_arena;

    /* --- DATA region sizing ---
     *
     * Total size of region 2 in bytes. Must be at least
     * (data filesz + bss size) from the ELF; the loader rejects
     * with VM_LOAD_ERR_DATA_SIZE_TOO_SMALL if not. The remainder
     * holds the guest's stack and any guest-side heap.
     *
     * sp will be initialized to (0x80000000 + region_data_size)
     * rounded down to 8-byte alignment. */
    uint32_t region_data_size;

    /* --- Shared region (passed through to region 3) ---
     *
     * Identical for every VM in the system. NULL/0 leaves region
     * 3 absent (any access from the guest traps). */
    void    *shared_base;        /* host pointer to shared memory */
    uint32_t shared_size;        /* size of shared memory in bytes */

} VmLoaderConfig;

/* ============================================================
 *  Load
 * ============================================================ */

/* Load a RISC-V RV32 ELF image into the given VmCpu.
 *
 *   cpu:       caller-declared VmCpu, will be populated. The
 *              caller should call vm_init(cpu, vm_id) before
 *              calling vm_load; vm_load fills in regions, PC,
 *              and sp but does not reset trap state or counters
 *              that vm_init already cleared.
 *
 *   elf_image: pointer to the ELF file bytes (in flash, RAM, or
 *              wherever). The loader reads through this pointer
 *              and does not modify it.
 *
 *   elf_size:  size of the ELF image in bytes.
 *
 *   cfg:       configuration (backing choices, RAM arena, shared
 *              region pointers).
 *
 *  On success, the cpu's regions, pc, and sp are set up; the VM
 *  is ready to run.
 *
 *  On failure, the cpu is left in an unspecified state — do not
 *  vm_step a VM that vm_load rejected. The bump arena may have
 *  had partial allocations made against it; the caller should
 *  bump_reset it if they want to retry.
 *
 *  Returns one of VmLoadResult. */
VmLoadResult vm_load(VmCpu *cpu,
                     const void *elf_image, size_t elf_size,
                     const VmLoaderConfig *cfg);

/* ============================================================
 *  Diagnostics
 *
 *  Convert a result code to a short human-readable string. Useful
 *  for error reporting and debug logs. Returns a pointer to a
 *  static string; do not free.
 * ============================================================ */

const char *vm_load_result_string(VmLoadResult r);

#endif /* VM_LOADER_H */
