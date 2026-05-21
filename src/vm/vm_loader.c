/* ============================================================
 *  vm_loader.c — implementation
 *  See vm/vm_loader.h for the public contract.
 *
 *  The loader handles two scenarios:
 *
 *    1. ELF on storage (SD card, trashdrive, etc.): host reads
 *       the file into a RAM buffer, hands the buffer to vm_load
 *       with code_backing=COPY_RAM and rodata_backing=COPY_RAM.
 *       After vm_load returns, the buffer can be freed — the
 *       loader has copied every byte the VM needs.
 *
 *    2. ELF embedded in flash: host passes a pointer into flash
 *       with code_backing=XIP and rodata_backing=XIP. The region
 *       descriptors point directly into flash; no RAM copy is
 *       made for code or rodata. The ELF must remain resident
 *       (which it does — it's in flash).
 *
 *  In both scenarios the DATA region (region 2) is always copied
 *  to RAM, because data is mutable and flash isn't.
 *
 *  Multi-byte ELF fields are decoded via memcpy into stack
 *  variables, not pointer casts. This avoids unaligned-access
 *  faults when the ELF buffer happens to be at an odd address
 *  — a real concern when the ELF was just read off storage into
 *  an unaligned malloc'd buffer.
 *
 *  Endianness: RV32 ELFs are always little-endian. We decode
 *  little-endian explicitly so the loader works on a big-endian
 *  host (unlikely in 2026, but the cost is zero).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_loader.h"
#include <string.h>

/* ============================================================
 *  ELF format constants — only what we need.
 *
 *  We deliberately don't pull in elf.h: it varies across host
 *  systems, includes hundreds of unused constants, and is
 *  inconsistent about types. We read structures byte-by-byte
 *  so we don't need its struct layouts either.
 * ============================================================ */

/* e_ident indices */
#define EI_MAG0      0
#define EI_MAG1      1
#define EI_MAG2      2
#define EI_MAG3      3
#define EI_CLASS     4
#define EI_DATA      5
#define EI_VERSION   6

/* e_ident values */
#define ELFMAG0       0x7F
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'
#define ELFCLASS32    1
#define ELFDATA2LSB   1
#define EV_CURRENT    1

/* e_type */
#define ET_EXEC       2

/* e_machine */
#define EM_RISCV    0xF3

/* p_type */
#define PT_NULL       0
#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3

/* p_flags */
#define PF_X     0x1
#define PF_W     0x2
#define PF_R     0x4

/* ============================================================
 *  ELF header offsets (within the on-disk structure)
 * ============================================================ */

#define ELF32_EHDR_SIZE        52u
#define ELF32_PHDR_SIZE        32u

/* ELF32_Ehdr field offsets */
#define EHDR_TYPE              16
#define EHDR_MACHINE           18
#define EHDR_VERSION           20
#define EHDR_ENTRY             24
#define EHDR_PHOFF             28
#define EHDR_PHENTSIZE         42
#define EHDR_PHNUM             44

/* ELF32_Phdr field offsets */
#define PHDR_TYPE              0
#define PHDR_OFFSET            4
#define PHDR_VADDR             8
#define PHDR_FILESZ           16
#define PHDR_MEMSZ            20
#define PHDR_FLAGS            24

/* ============================================================
 *  Little-endian readers — safe for any host alignment.
 * ============================================================ */

static inline uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    return v;
}

static inline uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = ((v >> 24) & 0x000000FFu) |
        ((v >>  8) & 0x0000FF00u) |
        ((v <<  8) & 0x00FF0000u) |
        ((v << 24) & 0xFF000000u);
#endif
    return v;
}

/* ============================================================
 *  Region routing
 * ============================================================ */

static inline uint32_t vaddr_to_region(uint32_t vaddr) {
    return vaddr >> 30;
}

/* ============================================================
 *  Per-segment validation. Returns OK or a specific error.
 * ============================================================ */

static VmLoadResult validate_pt_load(uint32_t vaddr, uint32_t filesz,
                                     uint32_t memsz, uint32_t flags) {
    uint32_t region = vaddr_to_region(vaddr);

    if (region == VM_REGION_SHARED) {
        /* The shared region is host-managed; no ELF segment
         * should land there. */
        return VM_LOAD_ERR_SEG_OUT_OF_RANGE;
    }

    /* Must fit within its region's 30-bit offset space. */
    uint32_t offset = vaddr & 0x3FFFFFFFu;
    if (memsz > 0x40000000u || offset > 0x40000000u - memsz) {
        return VM_LOAD_ERR_SEG_TOO_LARGE;
    }

    /* memsz must be at least filesz — the spec says so. */
    if (memsz < filesz) {
        return VM_LOAD_ERR_SEG_OUT_OF_RANGE;
    }

    bool is_writable = (flags & PF_W) != 0;

    if (region == VM_REGION_CODE || region == VM_REGION_RODATA) {
        if (is_writable) return VM_LOAD_ERR_SEG_REGION_MISMATCH;
        /* RO regions can't have BSS — that would imply zero-init
         * backing for read-only data, which makes no sense. */
        if (memsz > filesz) return VM_LOAD_ERR_SEG_BSS_IN_RO;
    }
    /* DATA region (writable): both writable=true and writable=false
     * are accepted (linker scripts vary). The loader treats it as
     * RAM regardless. */

    return VM_LOAD_OK;
}

/* ============================================================
 *  Apply one PT_LOAD segment.
 *
 *  - For CODE/RODATA in XIP: region->base points into the ELF.
 *  - For CODE/RODATA in COPY_RAM: allocate, memcpy, point at copy.
 *  - For DATA: allocate region_data_size bytes, copy filesz from
 *    ELF, zero (memsz - filesz) for BSS. Leave the rest for
 *    stack + heap.
 *
 *  Note: with the current strict-validation rules, each region
 *  (0, 1, 2) is expected to be populated by at most one PT_LOAD.
 *  If a linker script produced two PT_LOADs targeting the same
 *  region, the second would clobber the first's region descriptor.
 *  We don't currently detect this; stock linker scripts produce
 *  one PT_LOAD per region.
 * ============================================================ */

static VmLoadResult apply_pt_load(VmCpu *cpu,
                                  const uint8_t *elf_bytes,
                                  uint32_t offset, uint32_t filesz,
                                  uint32_t memsz, uint32_t vaddr,
                                  const VmLoaderConfig *cfg) {
    uint32_t region = vaddr_to_region(vaddr);
    VmRegion *r = &cpu->regions[region];

    if (region == VM_REGION_CODE || region == VM_REGION_RODATA) {
        VmBacking mode = (region == VM_REGION_CODE)
                       ? cfg->code_backing
                       : cfg->rodata_backing;

        if (mode == VM_BACKING_XIP) {
            const uint8_t *src = elf_bytes + offset;

            /* Code segment must be 2-byte aligned (C-extension
             * fetches half-words). If the ELF was downloaded
             * into an unaligned RAM buffer, in-place execution
             * isn't safe. */
            if (region == VM_REGION_CODE &&
                (((uintptr_t)src) & 0x1u) != 0) {
                return VM_LOAD_ERR_INVALID_ARG;
            }

            /* Cast away const: VmRegion shares one shape for
             * both RAM-backed (writable) and flash-backed
             * regions. The writable=false flag below ensures
             * any write attempt traps before reaching the
             * underlying memory. */
            r->base = (uint8_t *)src;
            r->length = filesz;
            r->writable = false;
        } else {
            /* COPY_RAM: allocate and copy. */
            void *dst = slab_alloc(cfg->ram_arena, filesz);
            if (!dst) return VM_LOAD_ERR_OUT_OF_RAM;
            memcpy(dst, elf_bytes + offset, filesz);
            r->base = (uint8_t *)dst;
            r->length = filesz;
            r->writable = false;
        }
    } else {
        /* DATA region: always RAM-backed, allocated at
         * cfg->region_data_size. */
        if (cfg->region_data_size < memsz) {
            return VM_LOAD_ERR_DATA_SIZE_TOO_SMALL;
        }

        void *dst = slab_alloc(cfg->ram_arena,
                               cfg->region_data_size);
        if (!dst) return VM_LOAD_ERR_OUT_OF_RAM;

        if (filesz > 0) {
            memcpy(dst, elf_bytes + offset, filesz);
        }
        if (memsz > filesz) {
            memset((uint8_t *)dst + filesz, 0, memsz - filesz);
        }
        /* Bytes from memsz to region_data_size are left
         * uninitialized; the stack writes over them as the
         * guest runs. */

        r->base = (uint8_t *)dst;
        r->length = cfg->region_data_size;
        r->writable = true;
    }

    return VM_LOAD_OK;
}

/* ============================================================
 *  Main loader entry point.
 * ============================================================ */

VmLoadResult vm_load(VmCpu *cpu,
                     const void *elf_image, size_t elf_size,
                     const VmLoaderConfig *cfg) {
    if (!cpu || !elf_image || !cfg) return VM_LOAD_ERR_INVALID_ARG;
    if (!cfg->ram_arena) return VM_LOAD_ERR_INVALID_ARG;
    if (cfg->region_data_size == 0) return VM_LOAD_ERR_INVALID_ARG;

    const uint8_t *elf = (const uint8_t *)elf_image;

    /* ===== ELF header validation ===== */

    if (elf_size < ELF32_EHDR_SIZE) return VM_LOAD_ERR_TRUNCATED;

    if (elf[EI_MAG0] != ELFMAG0 || elf[EI_MAG1] != ELFMAG1 ||
        elf[EI_MAG2] != ELFMAG2 || elf[EI_MAG3] != ELFMAG3) {
        return VM_LOAD_ERR_BAD_MAGIC;
    }
    if (elf[EI_CLASS] != ELFCLASS32)   return VM_LOAD_ERR_NOT_RV32;
    if (elf[EI_DATA]  != ELFDATA2LSB)  return VM_LOAD_ERR_NOT_LITTLE_ENDIAN;
    if (elf[EI_VERSION] != EV_CURRENT) return VM_LOAD_ERR_BAD_VERSION;

    uint16_t e_type     = rd16(elf + EHDR_TYPE);
    uint16_t e_machine  = rd16(elf + EHDR_MACHINE);
    uint32_t e_version  = rd32(elf + EHDR_VERSION);
    uint32_t e_entry    = rd32(elf + EHDR_ENTRY);
    uint32_t e_phoff    = rd32(elf + EHDR_PHOFF);
    uint16_t e_phentsz  = rd16(elf + EHDR_PHENTSIZE);
    uint16_t e_phnum    = rd16(elf + EHDR_PHNUM);

    if (e_machine != EM_RISCV)         return VM_LOAD_ERR_NOT_RISCV;
    if (e_type != ET_EXEC)             return VM_LOAD_ERR_NOT_EXECUTABLE;
    if (e_version != EV_CURRENT)       return VM_LOAD_ERR_BAD_VERSION;
    if (e_phentsz != ELF32_PHDR_SIZE)  return VM_LOAD_ERR_INVALID_ARG;

    /* Entry point must live in region 0 (CODE) and be 2-byte
     * aligned (C extension fetches half-words). */
    if (vaddr_to_region(e_entry) != VM_REGION_CODE) {
        return VM_LOAD_ERR_BAD_ENTRY;
    }
    if ((e_entry & 0x1u) != 0) {
        return VM_LOAD_ERR_BAD_ENTRY;
    }

    /* Program headers must fit in the file. */
    if (e_phnum == 0) return VM_LOAD_ERR_INVALID_ARG;
    uint64_t phdrs_end = (uint64_t)e_phoff +
                         (uint64_t)e_phnum * ELF32_PHDR_SIZE;
    if (phdrs_end > elf_size) return VM_LOAD_ERR_TRUNCATED;

    /* ===== Walk program headers =====
     *
     * Two passes:
     *   1. Validate every PT_LOAD; reject any PT_DYNAMIC/INTERP.
     *      Pass 1 doesn't modify cpu so a rejection leaves it
     *      essentially untouched.
     *   2. Apply PT_LOAD segments to the cpu. Failures here
     *      mean the cpu and arena are in a partial state and
     *      the caller should bump_reset before retrying.
     */

    /* Pass 1: validation only */
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (uint32_t)i * ELF32_PHDR_SIZE;

        uint32_t p_type   = rd32(ph + PHDR_TYPE);
        uint32_t p_offset = rd32(ph + PHDR_OFFSET);
        uint32_t p_vaddr  = rd32(ph + PHDR_VADDR);
        uint32_t p_filesz = rd32(ph + PHDR_FILESZ);
        uint32_t p_memsz  = rd32(ph + PHDR_MEMSZ);
        uint32_t p_flags  = rd32(ph + PHDR_FLAGS);

        if (p_type == PT_DYNAMIC) return VM_LOAD_ERR_DYNAMIC_FORBIDDEN;
        if (p_type == PT_INTERP)  return VM_LOAD_ERR_INTERP_FORBIDDEN;

        if (p_type != PT_LOAD) {
            /* PT_NOTE, PT_PHDR, PT_GNU_*, and anything else
             * non-load is accepted and ignored. */
            continue;
        }

        /* Empty PT_LOAD segments (filesz==0 && memsz==0) get
         * emitted by some linker scripts (notably when a PHDR
         * has no sections assigned to it) and contribute no
         * loadable content. Skip them rather than trying to
         * validate their meaningless vaddr/flags. */
        if (p_filesz == 0 && p_memsz == 0) {
            continue;
        }

        /* Segment file extent must lie within the ELF image. */
        if (p_filesz > 0) {
            uint64_t seg_end = (uint64_t)p_offset + (uint64_t)p_filesz;
            if (seg_end > elf_size) return VM_LOAD_ERR_TRUNCATED;
        }

        VmLoadResult vr = validate_pt_load(p_vaddr, p_filesz,
                                           p_memsz, p_flags);
        if (vr != VM_LOAD_OK) return vr;
    }

    /* Pass 2: apply. */
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (uint32_t)i * ELF32_PHDR_SIZE;

        uint32_t p_type   = rd32(ph + PHDR_TYPE);
        if (p_type != PT_LOAD) continue;

        uint32_t p_offset = rd32(ph + PHDR_OFFSET);
        uint32_t p_vaddr  = rd32(ph + PHDR_VADDR);
        uint32_t p_filesz = rd32(ph + PHDR_FILESZ);
        uint32_t p_memsz  = rd32(ph + PHDR_MEMSZ);

        /* Same skip rule as pass 1. */
        if (p_filesz == 0 && p_memsz == 0) continue;

        VmLoadResult ar = apply_pt_load(cpu, elf, p_offset,
                                        p_filesz, p_memsz,
                                        p_vaddr, cfg);
        if (ar != VM_LOAD_OK) return ar;
    }

    /* ===== Always allocate region 2 (DATA) if the ELF didn't ====
     *
     * Every VM needs a stack, which lives at the top of region 2.
     * If the ELF has no PT_LOAD targeting region 2 (which is normal
     * for hand-rolled code-only ELFs and many freestanding programs
     * that have no data or BSS), region 2 would otherwise be left
     * empty. Allocate it fresh so SP has somewhere to point. */
    if (cpu->regions[VM_REGION_DATA].length == 0) {
        void *dst = slab_alloc(cfg->ram_arena, cfg->region_data_size);
        if (!dst) return VM_LOAD_ERR_OUT_OF_RAM;
        memset(dst, 0, cfg->region_data_size);
        cpu->regions[VM_REGION_DATA].base = (uint8_t *)dst;
        cpu->regions[VM_REGION_DATA].length = cfg->region_data_size;
        cpu->regions[VM_REGION_DATA].writable = true;
    }

    /* ===== Shared region (host-managed, not from ELF) ===== */

    cpu->regions[VM_REGION_SHARED].base     = (uint8_t *)cfg->shared_base;
    cpu->regions[VM_REGION_SHARED].length   = cfg->shared_size;
    cpu->regions[VM_REGION_SHARED].writable = (cfg->shared_base != NULL);

    /* ===== Entry point and stack pointer ===== */

    cpu->pc = e_entry;

    /* sp = top of region 2, 8-byte aligned, growing down. */
    uint32_t sp_top = 0x80000000u + cfg->region_data_size;
    cpu->regs[VM_REG_SP] = sp_top & ~7u;

    return VM_LOAD_OK;
}

/* ============================================================
 *  Diagnostics
 * ============================================================ */

const char *vm_load_result_string(VmLoadResult r) {
    switch (r) {
    case VM_LOAD_OK:                       return "OK";
    case VM_LOAD_ERR_TRUNCATED:            return "ELF truncated";
    case VM_LOAD_ERR_BAD_MAGIC:            return "bad ELF magic";
    case VM_LOAD_ERR_NOT_RV32:             return "not RV32 (ELFCLASS32 required)";
    case VM_LOAD_ERR_NOT_LITTLE_ENDIAN:    return "not little-endian";
    case VM_LOAD_ERR_BAD_VERSION:          return "bad ELF version";
    case VM_LOAD_ERR_NOT_RISCV:            return "not RISC-V (EM_RISCV required)";
    case VM_LOAD_ERR_NOT_EXECUTABLE:       return "not executable (ET_EXEC required)";
    case VM_LOAD_ERR_BAD_ENTRY:            return "bad entry point";
    case VM_LOAD_ERR_DYNAMIC_FORBIDDEN:    return "PT_DYNAMIC forbidden";
    case VM_LOAD_ERR_INTERP_FORBIDDEN:     return "PT_INTERP forbidden";
    case VM_LOAD_ERR_SEG_OUT_OF_RANGE:     return "segment out of range";
    case VM_LOAD_ERR_SEG_REGION_MISMATCH:  return "segment writable bit doesn't match region";
    case VM_LOAD_ERR_SEG_BSS_IN_RO:        return "BSS in read-only segment";
    case VM_LOAD_ERR_SEG_TOO_LARGE:        return "segment exceeds region cap";
    case VM_LOAD_ERR_UNRESOLVED_RELOCS:    return "unresolved relocations present";
    case VM_LOAD_ERR_OUT_OF_RAM:           return "out of RAM in arena";
    case VM_LOAD_ERR_DATA_SIZE_TOO_SMALL:  return "region_data_size < data+bss";
    case VM_LOAD_ERR_INVALID_ARG:          return "invalid argument";
    }
    return "unknown error";
}
