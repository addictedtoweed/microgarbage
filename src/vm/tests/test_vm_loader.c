/* Tests for vm_loader.
 *
 * We don't need real RV32 ELF fixtures on disk — instead we
 * synthesize valid (and deliberately invalid) ELF images in
 * memory using a small helper. This lets us test every error
 * path with exactly the malformed shape we want. */

#include "test_runner.h"
#include "vm/vm_loader.h"
#include "vm/vm_core.h"
#include "memory/slab_stack.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Slab helper for tests
 *
 *  These tests previously used a bump allocator. With the slab
 *  migration, every test sets up a SlabAllocator over a local
 *  byte buffer. We adapt the bin layout to the buffer size: small
 *  buffers get a few small bins, larger buffers get a wider range.
 * ============================================================ */

static void test_slab_init(SlabAllocator *a, void *region, size_t bytes) {
    SlabConfig cfg = {0};
    /* Align the region pointer up to 8 bytes first; the slab needs
     * 8-aligned bin block storage. */
    uintptr_t base = (uintptr_t)region;
    uintptr_t aligned = (base + 7) & ~(uintptr_t)7;
    size_t adj = aligned - base;
    if (adj >= bytes) {
        /* Buffer too small even after alignment. Init a no-op slab
         * (zero buckets) so the struct is valid but all alloc
         * attempts return NULL. Tests that intentionally exercise
         * out-of-memory paths rely on this. */
        memset(a, 0, sizeof(*a));
        slab_init(a, region, bytes < 8 ? 8 : bytes, &cfg, slab_null_locker);
        return;
    }
    region = (void *)aligned;
    bytes -= adj;

    /* Choose a bin layout that fits in the available bytes. Real
     * required-bytes for each branch (computed via
     * slab_required_bytes):
     *   bins 0-9 x 2:  ~65 KB
     *   bins 0-8 x 2:  ~33 KB
     *   bins 0-7 x 2:  ~17 KB
     *   bins 0-4 x 2:  ~3.2 KB
     *   bins 0-1 x 2:  ~250 B */
    if (bytes >= 65536 + 1024) {
        /* Largest — bins 0-9 (up to 16 KB blocks). */
        for (int b = 0; b <= 9; b++) cfg.bucket_counts[b] = 2;
    } else if (bytes >= 33000 + 1024) {
        /* Large — bins 0-8 (up to 8 KB blocks). */
        for (int b = 0; b <= 8; b++) cfg.bucket_counts[b] = 2;
    } else if (bytes >= 17000 + 1024) {
        /* Medium — bins 0-7 (up to 4 KB blocks). */
        for (int b = 0; b <= 7; b++) cfg.bucket_counts[b] = 2;
    } else if (bytes >= 3200 + 256) {
        /* Small — up to 512 B blocks. */
        for (int b = 0; b <= 4; b++) cfg.bucket_counts[b] = 2;
    } else if (bytes >= 256) {
        /* Tiny. */
        cfg.bucket_counts[0] = 2;
        cfg.bucket_counts[1] = 2;
    } else {
        /* Buffer too small for any meaningful config. Init with
         * zero buckets — alloc attempts will fail with NULL.
         * Used by intentional-OOM tests. */
    }

    SlabResult r = slab_init(a, region, bytes, &cfg, slab_null_locker);
    if (r != SLAB_OK) {
        /* Silently zero the struct so callers don't crash on
         * the null locker. Tests that hit this path expect
         * subsequent allocations to fail. */
        memset(a, 0, sizeof(*a));
        a->locker = slab_null_locker;
    }
}

/* ============================================================
 *  ELF synthesizer
 *
 *  Produces an ELF32 little-endian image into a caller buffer.
 *  Layout: ELF header at offset 0, then phdrs immediately after,
 *  then segment file bytes packed after the phdrs.
 *
 *  Each call to elf_add_load_segment appends a PT_LOAD entry
 *  with the given vaddr, filesz, flags, and copies content into
 *  the segment data area. memsz = filesz unless set separately.
 * ============================================================ */

typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   total_size;     /* current end of written content */
    uint32_t entry;
    uint16_t machine;        /* defaults to EM_RISCV (0xF3) */
    uint16_t type;           /* defaults to ET_EXEC (2) */
    uint8_t  class;          /* defaults to ELFCLASS32 (1) */
    uint8_t  data_enc;       /* defaults to ELFDATA2LSB (1) */
    uint8_t  version;        /* defaults to EV_CURRENT (1) */
    uint16_t phnum;
    uint16_t phentsize;      /* defaults to 32 */
    size_t   phdrs_offset;   /* where phdrs begin (right after ehdr) */
    size_t   segments_offset; /* where segment data begins (after phdrs) */
} ElfBuilder;

#define EHDR_SIZE  52u
#define PHDR_SIZE  32u

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void elf_init(ElfBuilder *b, uint8_t *buf, size_t capacity,
                     uint32_t entry, uint16_t max_phnum) {
    memset(b, 0, sizeof(*b));
    memset(buf, 0, capacity);
    b->buf = buf;
    b->capacity = capacity;
    b->entry = entry;
    b->machine = 0xF3;   /* EM_RISCV */
    b->type = 2;         /* ET_EXEC */
    b->class = 1;        /* ELFCLASS32 */
    b->data_enc = 1;     /* ELFDATA2LSB */
    b->version = 1;      /* EV_CURRENT */
    b->phentsize = 32;
    b->phdrs_offset = EHDR_SIZE;
    b->segments_offset = EHDR_SIZE + (size_t)max_phnum * PHDR_SIZE;
    b->total_size = b->segments_offset;
}

/* Append a PT_LOAD segment. Returns the file offset where the
 * segment's bytes were written (caller can also pass content via
 * the 'content' parameter, which is memcpy'd in). */
static size_t elf_add_load(ElfBuilder *b,
                           uint32_t vaddr, uint32_t flags,
                           const void *content, uint32_t filesz,
                           uint32_t memsz) {
    size_t seg_offset = b->total_size;

    /* Write the phdr entry */
    uint8_t *ph = b->buf + b->phdrs_offset + (size_t)b->phnum * PHDR_SIZE;
    wr32(ph + 0,  1);          /* PT_LOAD */
    wr32(ph + 4,  (uint32_t)seg_offset);  /* p_offset */
    wr32(ph + 8,  vaddr);      /* p_vaddr */
    wr32(ph + 12, vaddr);      /* p_paddr */
    wr32(ph + 16, filesz);     /* p_filesz */
    wr32(ph + 20, memsz);      /* p_memsz */
    wr32(ph + 24, flags);      /* p_flags */
    wr32(ph + 28, 4);          /* p_align */

    /* Write the segment data */
    if (filesz > 0 && content) {
        memcpy(b->buf + seg_offset, content, filesz);
    }
    b->total_size += filesz;
    b->phnum++;
    return seg_offset;
}

/* Append a non-PT_LOAD phdr (PT_NOTE, etc.). Useful for testing
 * that the loader ignores them. */
static void elf_add_other(ElfBuilder *b, uint32_t type) {
    uint8_t *ph = b->buf + b->phdrs_offset + (size_t)b->phnum * PHDR_SIZE;
    wr32(ph + 0, type);
    /* Other fields zero. */
    b->phnum++;
}

/* Finalize the ELF: write the ehdr now that we know phnum. */
static void elf_finalize(ElfBuilder *b) {
    uint8_t *e = b->buf;
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
    e[4] = b->class;
    e[5] = b->data_enc;
    e[6] = b->version;
    /* e[7..15] zero already */
    wr16(e + 16, b->type);
    wr16(e + 18, b->machine);
    wr32(e + 20, b->version);
    wr32(e + 24, b->entry);
    wr32(e + 28, (uint32_t)b->phdrs_offset);   /* e_phoff */
    wr32(e + 32, 0);                            /* e_shoff (unused) */
    wr32(e + 36, 0);                            /* e_flags */
    wr16(e + 40, EHDR_SIZE);                   /* e_ehsize */
    wr16(e + 42, b->phentsize);                /* e_phentsize */
    wr16(e + 44, b->phnum);                    /* e_phnum */
    wr16(e + 46, 40);                          /* e_shentsize (unused) */
    wr16(e + 48, 0);                            /* e_shnum */
    wr16(e + 50, 0);                            /* e_shstrndx */
}

/* ============================================================
 *  Shared test fixtures
 * ============================================================ */

/* A tiny "program": four bytes representing a single RV32 NOP
 * (addi x0, x0, 0 = 0x00000013) repeated. Doesn't need to be
 * runnable — the loader just copies/points-at it. */
static const uint8_t SIMPLE_CODE[] = {
    0x13, 0x00, 0x00, 0x00,   /* addi x0, x0, 0 (nop) */
    0x13, 0x00, 0x00, 0x00,   /* nop */
    0x13, 0x00, 0x00, 0x00,   /* nop */
    0x13, 0x00, 0x00, 0x00,   /* nop */
};

/* Small read-only data */
static const uint8_t SIMPLE_RODATA[] = {
    'H', 'i', '!', 0
};

/* Small initialized data */
static const uint8_t SIMPLE_DATA[] = {
    0xDE, 0xAD, 0xBE, 0xEF
};

/* Build a valid ELF with code (region 0), rodata (region 1),
 * data (region 2). Returns total size in *out_size. */
static void build_simple_elf(uint8_t *buf, size_t capacity,
                             size_t *out_size) {
    ElfBuilder b;
    elf_init(&b, buf, capacity, /*entry=*/0x00000000, /*max_phnum=*/4);
    elf_add_load(&b, 0x00000000, 0x5,  /* PF_R | PF_X */
                 SIMPLE_CODE, sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_add_load(&b, 0x40000000, 0x4,  /* PF_R */
                 SIMPLE_RODATA, sizeof(SIMPLE_RODATA), sizeof(SIMPLE_RODATA));
    elf_add_load(&b, 0x80000000, 0x6,  /* PF_R | PF_W */
                 SIMPLE_DATA, sizeof(SIMPLE_DATA),
                 sizeof(SIMPLE_DATA) + 32);  /* +32 bytes of BSS */
    elf_finalize(&b);
    *out_size = b.total_size;
}

/* ============================================================
 *  Happy paths
 * ============================================================ */

static void test_load_simple_copy_ram(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);

    /* Set up host RAM for the VM */
    uint8_t vm_ram[32768];
    SlabAllocator arena;
    test_slab_init(&arena, vm_ram, sizeof(vm_ram));

    uint8_t shared_storage[256] = {0};

    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .code_backing   = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
        .ram_arena      = &arena,
        .region_data_size = 2048,
        .shared_base    = shared_storage,
        .shared_size    = sizeof(shared_storage),
    };

    VmLoadResult r = vm_load(&cpu, elf_buf, elf_size, &cfg);
    ASSERT_EQ_INT(VM_LOAD_OK, r);

    /* Regions populated */
    ASSERT_NOT_NULL(cpu.regions[VM_REGION_CODE].base);
    ASSERT_EQ_INT((int)sizeof(SIMPLE_CODE),
                  (int)cpu.regions[VM_REGION_CODE].length);
    ASSERT(!cpu.regions[VM_REGION_CODE].writable);

    ASSERT_NOT_NULL(cpu.regions[VM_REGION_RODATA].base);
    ASSERT_EQ_INT((int)sizeof(SIMPLE_RODATA),
                  (int)cpu.regions[VM_REGION_RODATA].length);
    ASSERT(!cpu.regions[VM_REGION_RODATA].writable);

    ASSERT_NOT_NULL(cpu.regions[VM_REGION_DATA].base);
    ASSERT_EQ_INT(2048, (int)cpu.regions[VM_REGION_DATA].length);
    ASSERT(cpu.regions[VM_REGION_DATA].writable);

    /* Shared region passthrough */
    ASSERT_EQ_PTR(shared_storage, cpu.regions[VM_REGION_SHARED].base);
    ASSERT_EQ_INT(256, (int)cpu.regions[VM_REGION_SHARED].length);

    /* PC and SP */
    ASSERT_EQ_INT(0, (int)cpu.pc);
    ASSERT_EQ_INT((int)(0x80000000u + 2048), (int)cpu.regs[VM_REG_SP]);

    /* Code copied — first instruction is the nop we put in */
    ASSERT_EQ_INT(0x13, cpu.regions[VM_REGION_CODE].base[0]);

    /* Rodata copied */
    ASSERT_EQ_INT('H', cpu.regions[VM_REGION_RODATA].base[0]);
    ASSERT_EQ_INT('i', cpu.regions[VM_REGION_RODATA].base[1]);

    /* Data: filesz bytes copied, BSS zeroed */
    ASSERT_EQ_INT(0xDE, cpu.regions[VM_REGION_DATA].base[0]);
    ASSERT_EQ_INT(0xEF, cpu.regions[VM_REGION_DATA].base[3]);
    ASSERT_EQ_INT(0, cpu.regions[VM_REGION_DATA].base[4]);    /* BSS */
    ASSERT_EQ_INT(0, cpu.regions[VM_REGION_DATA].base[35]);   /* BSS end */
}

static void test_load_simple_xip(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);

    uint8_t vm_ram[32768];
    SlabAllocator arena;
    test_slab_init(&arena, vm_ram, sizeof(vm_ram));

    VmCpu cpu;
    vm_init(&cpu, 1);

    VmLoaderConfig cfg = {
        .code_backing   = VM_BACKING_XIP,
        .rodata_backing = VM_BACKING_XIP,
        .ram_arena      = &arena,
        .region_data_size = 1024,
        .shared_base    = NULL,
        .shared_size    = 0,
    };

    VmLoadResult r = vm_load(&cpu, elf_buf, elf_size, &cfg);
    ASSERT_EQ_INT(VM_LOAD_OK, r);

    /* In XIP, code/rodata bases point INTO the ELF buffer, not
     * into the arena. So they should be within elf_buf's range. */
    uint8_t *code_base = cpu.regions[VM_REGION_CODE].base;
    uint8_t *rodata_base = cpu.regions[VM_REGION_RODATA].base;
    ASSERT(code_base >= elf_buf && code_base < elf_buf + elf_size);
    ASSERT(rodata_base >= elf_buf && rodata_base < elf_buf + elf_size);

    /* Data is still RAM-backed even in XIP mode */
    uint8_t *data_base = cpu.regions[VM_REGION_DATA].base;
    ASSERT(data_base >= vm_ram && data_base < vm_ram + sizeof(vm_ram));

    /* Shared region absent when shared_base is NULL */
    ASSERT_NULL(cpu.regions[VM_REGION_SHARED].base);
    ASSERT(!cpu.regions[VM_REGION_SHARED].writable);
}

static void test_load_accepts_pt_note(void) {
    /* Non-LOAD program headers should be ignored. */
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0x00000000, 5);
    elf_add_load(&b, 0x00000000, 0x5,
                 SIMPLE_CODE, sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_add_other(&b, 4);   /* PT_NOTE */
    elf_add_load(&b, 0x80000000, 0x6,
                 SIMPLE_DATA, sizeof(SIMPLE_DATA), sizeof(SIMPLE_DATA));
    elf_add_other(&b, 0x6474e551);   /* PT_GNU_STACK */
    elf_finalize(&b);

    uint8_t vm_ram[32768];
    SlabAllocator arena;
    test_slab_init(&arena, vm_ram, sizeof(vm_ram));

    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .code_backing   = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
        .ram_arena      = &arena,
        .region_data_size = 1024,
    };
    ASSERT_EQ_INT(VM_LOAD_OK, vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

/* ============================================================
 *  Validation rejections
 * ============================================================ */

static void test_reject_truncated(void) {
    uint8_t elf_buf[10] = {0};
    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 64,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_TRUNCATED,
                  vm_load(&cpu, elf_buf, sizeof(elf_buf), &cfg));
}

static void test_reject_bad_magic(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);
    /* Corrupt the magic */
    elf_buf[0] = 'X';

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_BAD_MAGIC,
                  vm_load(&cpu, elf_buf, elf_size, &cfg));
}

static void test_reject_wrong_class(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);
    elf_buf[4] = 2;   /* ELFCLASS64 instead of ELFCLASS32 */

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_NOT_RV32,
                  vm_load(&cpu, elf_buf, elf_size, &cfg));
}

static void test_reject_wrong_endian(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);
    elf_buf[5] = 2;   /* ELFDATA2MSB */

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_NOT_LITTLE_ENDIAN,
                  vm_load(&cpu, elf_buf, elf_size, &cfg));
}

static void test_reject_wrong_machine(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0, 1);
    b.machine = 0xF7;   /* not EM_RISCV */
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_NOT_RISCV,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_not_executable(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0, 1);
    b.type = 3;   /* ET_DYN — not what we accept */
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_NOT_EXECUTABLE,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_pt_dynamic(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0, 2);
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_add_other(&b, 2);   /* PT_DYNAMIC */
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_DYNAMIC_FORBIDDEN,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_pt_interp(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0, 2);
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_add_other(&b, 3);   /* PT_INTERP */
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_INTERP_FORBIDDEN,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_writable_code(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0, 1);
    /* PF_X | PF_W (executable and writable) — region 0 is RO */
    elf_add_load(&b, 0x00000000, 0x3, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_SEG_REGION_MISMATCH,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_entry_not_in_code(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf),
             /*entry=*/0x40000000,   /* in rodata, not code */
             1);
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_BAD_ENTRY,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_entry_misaligned(void) {
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf),
             /*entry=*/0x00000001,   /* odd byte */
             1);
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_BAD_ENTRY,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_bss_in_rodata(void) {
    /* rodata with memsz > filesz — would imply zero-init RO data */
    uint8_t elf_buf[1024];
    ElfBuilder b;
    elf_init(&b, elf_buf, sizeof(elf_buf), 0, 2);
    elf_add_load(&b, 0x00000000, 0x5, SIMPLE_CODE,
                 sizeof(SIMPLE_CODE), sizeof(SIMPLE_CODE));
    /* rodata: filesz=4, memsz=8 — illegal */
    elf_add_load(&b, 0x40000000, 0x4, SIMPLE_RODATA, 4, 8);
    elf_finalize(&b);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_SEG_BSS_IN_RO,
                  vm_load(&cpu, elf_buf, b.total_size, &cfg));
}

static void test_reject_data_size_too_small(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);
    /* simple ELF wants 4 bytes data + 32 bytes bss = 36; we'll
     * give it only 8. */

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 8,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_DATA_SIZE_TOO_SMALL,
                  vm_load(&cpu, elf_buf, elf_size, &cfg));
}

static void test_reject_arena_exhausted(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);

    /* Arena too small for the data region alone */
    SlabAllocator arena;
    uint8_t ram[16];
    test_slab_init(&arena, ram, sizeof(ram));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 1024,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_ERR_OUT_OF_RAM,
                  vm_load(&cpu, elf_buf, elf_size, &cfg));
}

/* ============================================================
 *  Sanity / API checks
 * ============================================================ */

static void test_reject_null_args(void) {
    uint8_t buf[4096];
    SlabAllocator arena;
    test_slab_init(&arena, buf, sizeof(buf));
    VmCpu cpu;
    vm_init(&cpu, 0);

    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 32,
    };

    ASSERT_EQ_INT(VM_LOAD_ERR_INVALID_ARG, vm_load(NULL, buf, 64, &cfg));
    ASSERT_EQ_INT(VM_LOAD_ERR_INVALID_ARG, vm_load(&cpu, NULL, 64, &cfg));
    ASSERT_EQ_INT(VM_LOAD_ERR_INVALID_ARG, vm_load(&cpu, buf, 64, NULL));
}

static void test_result_strings_exist(void) {
    /* Every result code should have a non-NULL string. */
    ASSERT_NOT_NULL(vm_load_result_string(VM_LOAD_OK));
    ASSERT_NOT_NULL(vm_load_result_string(VM_LOAD_ERR_BAD_MAGIC));
    ASSERT_NOT_NULL(vm_load_result_string(VM_LOAD_ERR_OUT_OF_RAM));
    ASSERT_NOT_NULL(vm_load_result_string((VmLoadResult)999));  /* unknown */
}

/* ============================================================
 *  Reload cycle — confirms the dynamic-loading workflow works
 *
 *  Previously this checked bump_used before/after bump_reset.
 *  In the slab world, the equivalent is: load → free all the
 *  cpu's regions explicitly → load again. The slab returns the
 *  freed bins, so the second load uses the same memory. We
 *  verify by checking total bytes_in_use returns to the same
 *  level after the second load.
 * ============================================================ */

static void test_reload_after_slab_free(void) {
    uint8_t elf_buf[1024];
    size_t elf_size;
    build_simple_elf(elf_buf, sizeof(elf_buf), &elf_size);

    SlabAllocator arena;
    uint8_t ram[32768];
    test_slab_init(&arena, ram, sizeof(ram));

    /* First load */
    VmCpu cpu;
    vm_init(&cpu, 0);
    VmLoaderConfig cfg = {
        .ram_arena = &arena, .region_data_size = 512,
        .code_backing = VM_BACKING_COPY_RAM,
        .rodata_backing = VM_BACKING_COPY_RAM,
    };
    ASSERT_EQ_INT(VM_LOAD_OK, vm_load(&cpu, elf_buf, elf_size, &cfg));
    size_t used_after_first = arena.total_bytes_in_use;
    ASSERT(used_after_first > 0);

    /* Free everything the loader allocated. slab_free handles
     * NULL and foreign pointers gracefully, so the loop just
     * walks each region. */
    for (uint32_t i = 0; i < VM_REGION_COUNT; i++) {
        if (cpu.regions[i].base) {
            slab_free(&arena, cpu.regions[i].base);
            cpu.regions[i].base = NULL;
            cpu.regions[i].length = 0;
        }
    }

    /* Re-init the cpu and reload. With the slab returning
     * freed bins to their freelists, the second load should
     * consume the same bytes. */
    vm_init(&cpu, 0);
    ASSERT_EQ_INT(VM_LOAD_OK, vm_load(&cpu, elf_buf, elf_size, &cfg));
    ASSERT_EQ_INT((int)used_after_first, (int)arena.total_bytes_in_use);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_loader");

    /* Happy paths */
    RUN(test_load_simple_copy_ram);
    RUN(test_load_simple_xip);
    RUN(test_load_accepts_pt_note);

    /* Header rejections */
    RUN(test_reject_truncated);
    RUN(test_reject_bad_magic);
    RUN(test_reject_wrong_class);
    RUN(test_reject_wrong_endian);
    RUN(test_reject_wrong_machine);
    RUN(test_reject_not_executable);

    /* Segment rejections */
    RUN(test_reject_pt_dynamic);
    RUN(test_reject_pt_interp);
    RUN(test_reject_writable_code);
    RUN(test_reject_entry_not_in_code);
    RUN(test_reject_entry_misaligned);
    RUN(test_reject_bss_in_rodata);

    /* Resource rejections */
    RUN(test_reject_data_size_too_small);
    RUN(test_reject_arena_exhausted);

    /* API */
    RUN(test_reject_null_args);
    RUN(test_result_strings_exist);

    /* Lifecycle */
    RUN(test_reload_after_slab_free);

    return TEST_SUITE_RESULT();
}
