/* ============================================================
 *  vm_core.c — VM core implementation
 *  See vm/vm_core.h for the public contract.
 *
 *  This file currently provides:
 *    - Lifecycle:    vm_init, vm_reset
 *    - Memory:       vm_translate_*, vm_read_u32, vm_write_u32,
 *                    vm_copy_*_guest
 *    - Dispatcher:   vm_step (fetch + decode skeleton; instruction
 *                    semantics added in subsequent chunks)
 *
 *  The dispatcher today only handles the fetch path and reports
 *  TRAP_ILLEGAL_INSTR for any real instruction. Each subsequent
 *  chunk fills in instruction classes (RV32I ALU, loads/stores,
 *  branches/jumps, M extension, C extension expansion).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

/* This file is the interpreter hot path — fetch/decode/execute runs
 * for every guest instruction. Size-optimized builds compile the
 * rest of the host at -Os; we pin THIS translation unit to -O2 so
 * the dispatch loop stays fast regardless of the command-line -O
 * level. (GCC/Clang extension; supported by every toolchain we use:
 * mingw, the riscv-none-elf cross, and desktop gcc/clang.) If a
 * future compiler lacks it, the pragma is ignored and the file just
 * inherits the command-line level — correct, just not speed-pinned. */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC optimize ("O2")
#endif

#include "vm/vm_core.h"
#include <string.h>

/* ============================================================
 *  Lifecycle
 * ============================================================ */

void vm_init(VmCpu *cpu, uint16_t vm_id) {
    if (!cpu) return;
    memset(cpu, 0, sizeof(*cpu));
    cpu->vm_id = vm_id;
    /* 0 is a valid vm_id, so the "not waiting on a child" sentinel
     * must be explicit — memset's zero would mean "waiting on vm 0". */
    cpu->block_child_vm = UINT16_MAX;
}

void vm_reset(VmCpu *cpu) {
    if (!cpu) return;

    VmRegion saved_regions[VM_REGION_COUNT];
    VmRegion saved_l2 = cpu->l2_shared;
    memcpy(saved_regions, cpu->regions, sizeof(saved_regions));
    uint16_t saved_id = cpu->vm_id;

    memset(cpu, 0, sizeof(*cpu));

    memcpy(cpu->regions, saved_regions, sizeof(saved_regions));
    cpu->l2_shared = saved_l2;
    cpu->vm_id = saved_id;
    cpu->block_child_vm = UINT16_MAX;
}

/* ============================================================
 *  Memory access — internal fast paths
 *
 *  The dispatcher calls these inline on every load/store/fetch.
 *  They do the top-2-bits region split, bounds-check, and
 *  return a host pointer (or NULL on fault). On fault they set
 *  trap state on the CPU; the caller (the dispatcher) is
 *  expected to check trap_cause and bail.
 * ============================================================ */

/* Pick the (region, offset) pair for an address. Most regions are a
 * straight 2-bit selector / 30-bit offset split. SHARED is split a
 * second time at bit 29 so we can carry an L2 sub-region (PSRAM on
 * the MCU) alongside the existing system-wide mailbox storage:
 *
 *     0xC000_0000 - 0xDFFF_FFFF  -> regions[SHARED]  (L1; bit 29 = 0)
 *     0xE000_0000 - 0xFFFF_FFFF  -> l2_shared        (L2; bit 29 = 1)
 *
 * When l2_shared.length is 0 (the default; embedder didn't install
 * an L2 backing), an access to the upper half just fails the bounds
 * check and traps — matching the pre-split behavior bit-for-bit. */
static inline VmRegion *xlat_pick(VmCpu *cpu, uint32_t addr, uint32_t *out_off) {
    uint32_t region = addr >> 30;
    if (region == VM_REGION_SHARED) {
        if (addr & 0x20000000u) {
            *out_off = addr & 0x1FFFFFFFu;
            return &cpu->l2_shared;
        }
        *out_off = addr & 0x1FFFFFFFu;
        return &cpu->regions[VM_REGION_SHARED];
    }
    *out_off = addr & 0x3FFFFFFFu;
    return &cpu->regions[region];
}

static inline const uint8_t *xlat_read(VmCpu *cpu,
                                       uint32_t addr,
                                       uint32_t size,
                                       VmTrapCause fault_cause) {
    uint32_t offset;
    VmRegion *r = xlat_pick(cpu, addr, &offset);

    /* Region must be populated and the access must fit. */
    if (r->length == 0 || offset > r->length || size > r->length - offset) {
        cpu->trap_cause = fault_cause;
        cpu->trap_pc    = cpu->pc;
        cpu->trap_addr  = addr;
        return NULL;
    }
    return r->base + offset;
}

static inline uint8_t *xlat_write(VmCpu *cpu,
                                  uint32_t addr,
                                  uint32_t size) {
    uint32_t offset;
    VmRegion *r = xlat_pick(cpu, addr, &offset);

    if (r->length == 0 || offset > r->length || size > r->length - offset) {
        cpu->trap_cause = TRAP_STORE_FAULT;
        cpu->trap_pc    = cpu->pc;
        cpu->trap_addr  = addr;
        return NULL;
    }
    if (!r->writable) {
        cpu->trap_cause = TRAP_STORE_RO;
        cpu->trap_pc    = cpu->pc;
        cpu->trap_addr  = addr;
        return NULL;
    }
    return r->base + offset;
}

/* ============================================================
 *  Memory access — public API
 *
 *  Used by ECALL handlers, debuggers, and tests. The dispatcher
 *  uses the inline static versions above for the hot path.
 * ============================================================ */

const void *vm_translate_read(VmCpu *cpu, uint32_t addr, uint32_t size) {
    if (!cpu || size == 0) return NULL;
    return xlat_read(cpu, addr, size, TRAP_LOAD_FAULT);
}

void *vm_translate_write(VmCpu *cpu, uint32_t addr, uint32_t size) {
    if (!cpu || size == 0) return NULL;
    return xlat_write(cpu, addr, size);
}

uint32_t vm_read_u32(VmCpu *cpu, uint32_t addr) {
    if (!cpu) return 0;
    if (addr & 0x3u) {
        cpu->trap_cause = TRAP_LOAD_MISALIGNED;
        cpu->trap_pc    = cpu->pc;
        cpu->trap_addr  = addr;
        return 0;
    }
    const uint8_t *p = xlat_read(cpu, addr, 4, TRAP_LOAD_FAULT);
    if (!p) return 0;

    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

bool vm_write_u32(VmCpu *cpu, uint32_t addr, uint32_t value) {
    if (!cpu) return false;
    if (addr & 0x3u) {
        cpu->trap_cause = TRAP_STORE_MISALIGNED;
        cpu->trap_pc    = cpu->pc;
        cpu->trap_addr  = addr;
        return false;
    }
    uint8_t *p = xlat_write(cpu, addr, 4);
    if (!p) return false;
    memcpy(p, &value, 4);
    return true;
}

bool vm_copy_from_guest(VmCpu *cpu, void *dst, uint32_t guest_src, uint32_t n) {
    if (!cpu || !dst || n == 0) return false;
    const uint8_t *p = xlat_read(cpu, guest_src, n, TRAP_LOAD_FAULT);
    if (!p) return false;
    memcpy(dst, p, n);
    return true;
}

bool vm_copy_to_guest(VmCpu *cpu, uint32_t guest_dst, const void *src, uint32_t n) {
    if (!cpu || !src || n == 0) return false;
    uint8_t *p = xlat_write(cpu, guest_dst, n);
    if (!p) return false;
    memcpy(p, src, n);
    return true;
}

/* ============================================================
 *  Instruction fetch
 *
 *  RV32IMC instructions are either 32-bit (low 2 bits == 0b11)
 *  or 16-bit compressed (any other low 2 bits).
 *
 *  Reads the next halfword at PC; if it's a 32-bit instruction
 *  reads the next halfword too. Returns full instruction bits
 *  in *out_insn and size (2 or 4) in *out_size.
 * ============================================================ */

static inline bool fetch_instruction(VmCpu *cpu,
                                     uint32_t *out_insn,
                                     uint32_t *out_size) {
    if (cpu->pc & 0x1u) {
        cpu->trap_cause = TRAP_INSTR_MISALIGNED;
        cpu->trap_pc    = cpu->pc;
        cpu->trap_addr  = cpu->pc;
        return false;
    }

    const uint8_t *p = xlat_read(cpu, cpu->pc, 2, TRAP_INSTR_FETCH_FAULT);
    if (!p) return false;

    uint16_t lo;
    memcpy(&lo, p, 2);

    if ((lo & 0x3u) == 0x3u) {
        p = xlat_read(cpu, cpu->pc + 2, 2, TRAP_INSTR_FETCH_FAULT);
        if (!p) return false;
        uint16_t hi;
        memcpy(&hi, p, 2);
        *out_insn = ((uint32_t)hi << 16) | (uint32_t)lo;
        *out_size = 4;
    } else {
        *out_insn = (uint32_t)lo;
        *out_size = 2;
    }
    return true;
}

/* ============================================================
 *  Instruction decode helpers
 *
 *  RV32 encodings are dense and share a few common bit ranges
 *  across formats. These accessors extract them cleanly:
 *
 *    opcode = insn[6:0]
 *    rd     = insn[11:7]
 *    funct3 = insn[14:12]
 *    rs1    = insn[19:15]
 *    rs2    = insn[24:20]
 *    funct7 = insn[31:25]
 *
 *  Immediate-format extractors sign-extend the assembled value
 *  to int32_t using shift-then-arithmetic-shift, which is the
 *  portable way that avoids implementation-defined behavior on
 *  signed shifts.
 * ============================================================ */

static inline uint32_t opcode_of(uint32_t insn) { return insn & 0x7Fu; }
static inline uint32_t rd_of(uint32_t insn)     { return (insn >> 7) & 0x1Fu; }
static inline uint32_t funct3_of(uint32_t insn) { return (insn >> 12) & 0x7u; }
static inline uint32_t rs1_of(uint32_t insn)    { return (insn >> 15) & 0x1Fu; }
static inline uint32_t rs2_of(uint32_t insn)    { return (insn >> 20) & 0x1Fu; }
static inline uint32_t funct7_of(uint32_t insn) { return (insn >> 25) & 0x7Fu; }

/* I-type 12-bit immediate, sign-extended to int32_t.
 *   bits[31:20] of instruction → signed 12-bit value */
static inline int32_t imm_i(uint32_t insn) {
    /* Move the 12-bit field into the high bits of a uint32, then
     * arithmetic-shift right as a signed int32. This sign-extends
     * portably regardless of how the C implementation handles
     * shifts of negative values. */
    return ((int32_t)insn) >> 20;
}

/* S-type 12-bit immediate (used by stores). Split across two
 * fields: imm[11:5] at bits[31:25], imm[4:0] at bits[11:7]. */
static inline int32_t imm_s(uint32_t insn) {
    uint32_t hi = (insn >> 25) & 0x7Fu;
    uint32_t lo = (insn >> 7)  & 0x1Fu;
    uint32_t combined = (hi << 5) | lo;
    /* Sign-extend the 12-bit value: shift left to put bit 11 at
     * bit 31, then arithmetic shift right. */
    return ((int32_t)(combined << 20)) >> 20;
}

/* B-type 13-bit immediate (used by branches). The low bit is
 * implicitly zero (branches are 2-byte aligned), so the encoded
 * field holds bits [12:1].
 *
 *   imm[12]   = insn[31]
 *   imm[10:5] = insn[30:25]
 *   imm[4:1]  = insn[11:8]
 *   imm[11]   = insn[7]
 *   imm[0]    = 0 */
static inline int32_t imm_b(uint32_t insn) {
    uint32_t b12 = (insn >> 31) & 0x1u;
    uint32_t b11 = (insn >> 7)  & 0x1u;
    uint32_t b10_5 = (insn >> 25) & 0x3Fu;
    uint32_t b4_1  = (insn >> 8)  & 0xFu;
    uint32_t combined = (b12 << 12) | (b11 << 11) |
                        (b10_5 << 5) | (b4_1 << 1);
    /* Sign-extend a 13-bit value: shift to put bit 12 at bit 31,
     * then arithmetic shift back. */
    return ((int32_t)(combined << 19)) >> 19;
}

/* J-type 21-bit immediate (used by JAL). Low bit is implicitly
 * zero; encoded field holds bits [20:1].
 *
 *   imm[20]    = insn[31]
 *   imm[10:1]  = insn[30:21]
 *   imm[11]    = insn[20]
 *   imm[19:12] = insn[19:12]
 *   imm[0]     = 0 */
static inline int32_t imm_j(uint32_t insn) {
    uint32_t b20    = (insn >> 31) & 0x1u;
    uint32_t b19_12 = (insn >> 12) & 0xFFu;
    uint32_t b11    = (insn >> 20) & 0x1u;
    uint32_t b10_1  = (insn >> 21) & 0x3FFu;
    uint32_t combined = (b20 << 20) | (b19_12 << 12) |
                        (b11 << 11) | (b10_1 << 1);
    /* Sign-extend a 21-bit value. */
    return ((int32_t)(combined << 11)) >> 11;
}

/* U-type 20-bit upper immediate, sign-extended and placed at
 * bits [31:12] of the result (low 12 bits zero). LUI and AUIPC. */
static inline int32_t imm_u(uint32_t insn) {
    return (int32_t)(insn & 0xFFFFF000u);
}

/* Shift amount from I-type encoding: bits [24:20]. */
static inline uint32_t shamt_of(uint32_t insn) {
    return (insn >> 20) & 0x1Fu;
}

/* ============================================================
 *  Portable arithmetic-shift-right of a 32-bit value
 *
 *  C's right-shift on signed types is implementation-defined for
 *  negative values. Every compiler we care about does arithmetic
 *  shift, but we don't depend on that — we compute the sign-
 *  extended result explicitly.
 * ============================================================ */

static inline uint32_t sra32(uint32_t v, uint32_t shift) {
    uint32_t s = shift & 0x1Fu;
    if (s == 0) return v;
    uint32_t result = v >> s;
    if (v & 0x80000000u) {
        /* Set the high `s` bits to 1. */
        result |= (uint32_t)~((1u << (32 - s)) - 1u);
    }
    return result;
}

/* ============================================================
 *  CSR access
 *
 *  Only a handful of CSRs are implemented (see vm_core.h). All
 *  others read as 0 and ignore writes — a deliberate shim so
 *  stock crt0 code that touches CSRs unconditionally doesn't
 *  trap.
 *
 *  csr_read returns the current value (0 for unknown CSRs).
 *  csr_write applies the new value where supported (no-op for
 *  unknown CSRs). Neither traps.
 * ============================================================ */

#define CSR_CYCLE     0xC00u
#define CSR_INSTRET   0xC02u
#define CSR_CYCLEH    0xC80u
#define CSR_INSTRETH  0xC82u
#define CSR_MHARTID   0xF14u

static inline uint32_t csr_read(VmCpu *cpu, uint32_t csr) {
    switch (csr) {
    case CSR_CYCLE:
    case CSR_INSTRET:
        return (uint32_t)(cpu->instructions_retired & 0xFFFFFFFFu);
    case CSR_CYCLEH:
    case CSR_INSTRETH:
        return (uint32_t)(cpu->instructions_retired >> 32);
    case CSR_MHARTID:
        return (uint32_t)cpu->vm_id;
    default:
        return 0;
    }
}

static inline void csr_write(VmCpu *cpu, uint32_t csr, uint32_t value) {
    (void)cpu; (void)csr; (void)value;
    /* All supported CSRs are read-only or counters that the
     * core itself maintains; writes are silently ignored. */
}

/* ============================================================
 *  C extension expander
 *
 *  Each 16-bit compressed instruction maps to exactly one 32-bit
 *  instruction with identical semantics. We decode the compressed
 *  encoding's quadrant + funct fields, extract the immediate using
 *  the format's specific bit shuffle, and assemble the 32-bit
 *  equivalent. The dispatcher then runs the 32-bit form through
 *  the normal switch.
 *
 *  Returns the 32-bit instruction on success, or 0 (an illegal
 *  encoding under the existing decoder) when the compressed form
 *  is reserved/illegal. The caller (execute_one) detects this by
 *  checking for the all-zeros result.
 *
 *  The compressed register fields rs1', rs2', rd' are 3-bit
 *  indices into x8..x15. We map them by ORing 8 onto the value.
 *
 *  The immediate-bit shuffles are different for every encoding and
 *  hand-coded per the RISC-V C spec tables. Each is commented in
 *  place.
 * ============================================================ */

/* Build a 32-bit R-type instruction. */
static inline uint32_t mk_r(uint32_t f7, uint32_t rs2,
                            uint32_t rs1, uint32_t f3,
                            uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | op;
}

/* Build a 32-bit I-type instruction. imm12 is the low 12 bits of
 * the immediate (signed); we don't sign-extend here, the
 * dispatcher's imm_i does that. */
static inline uint32_t mk_i(uint32_t imm12, uint32_t rs1,
                            uint32_t f3, uint32_t rd, uint32_t op) {
    return ((imm12 & 0xFFFu) << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | op;
}

/* Build a 32-bit S-type instruction. imm12 is the low 12 bits. */
static inline uint32_t mk_s(uint32_t imm12, uint32_t rs2, uint32_t rs1,
                            uint32_t f3, uint32_t op) {
    uint32_t hi = (imm12 >> 5) & 0x7Fu;
    uint32_t lo = imm12 & 0x1Fu;
    return (hi << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (lo << 7) | op;
}

/* Build a 32-bit B-type instruction. imm is the signed byte
 * offset (must be even). */
static inline uint32_t mk_b(uint32_t imm, uint32_t rs2, uint32_t rs1,
                            uint32_t f3, uint32_t op) {
    uint32_t b12 = (imm >> 12) & 0x1u;
    uint32_t b11 = (imm >> 11) & 0x1u;
    uint32_t b10_5 = (imm >> 5) & 0x3Fu;
    uint32_t b4_1 = (imm >> 1) & 0xFu;
    return (b12 << 31) | (b10_5 << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (b4_1 << 8) | (b11 << 7) | op;
}

/* Build a 32-bit U-type instruction. imm is placed in upper 20 bits. */
static inline uint32_t mk_u(uint32_t imm, uint32_t rd, uint32_t op) {
    return (imm & 0xFFFFF000u) | (rd << 7) | op;
}

/* Build a 32-bit J-type instruction. imm is the signed byte offset. */
static inline uint32_t mk_j(uint32_t imm, uint32_t rd, uint32_t op) {
    uint32_t b20 = (imm >> 20) & 0x1u;
    uint32_t b19_12 = (imm >> 12) & 0xFFu;
    uint32_t b11 = (imm >> 11) & 0x1u;
    uint32_t b10_1 = (imm >> 1) & 0x3FFu;
    return (b20 << 31) | (b10_1 << 21) | (b11 << 20) | (b19_12 << 12) |
           (rd << 7) | op;
}

/* Sign-extend an N-bit value held in low bits of a uint32_t to int32_t. */
static inline int32_t sign_extend(uint32_t v, int bits) {
    uint32_t shift = (uint32_t)(32 - bits);
    return ((int32_t)(v << shift)) >> shift;
}

/* Compressed-register decoder: 3-bit field → x8..x15. */
static inline uint32_t creg(uint32_t f3) { return f3 + 8; }

/* The expander. c is the 16-bit compressed instruction; returns
 * the equivalent 32-bit instruction or 0 on illegal/reserved. */
static uint32_t expand_compressed(uint16_t c) {
    uint32_t quadrant = c & 0x3u;
    uint32_t f3       = (c >> 13) & 0x7u;

    if (quadrant == 0) {
        /* === QUADRANT 0 === */
        switch (f3) {
        case 0x0: {
            /* C.ADDI4SPN: addi rd', x2, nzuimm[9:2]
             *   c[12:11] = nzuimm[5:4]
             *   c[10:7]  = nzuimm[9:6]
             *   c[6]     = nzuimm[2]
             *   c[5]     = nzuimm[3]
             * Zero immediate is reserved (illegal). */
            uint32_t rd_p = creg((c >> 2) & 0x7u);
            uint32_t imm =
                (((c >> 11) & 0x3u) << 4) |
                (((c >> 7)  & 0xFu) << 6) |
                (((c >> 6)  & 0x1u) << 2) |
                (((c >> 5)  & 0x1u) << 3);
            if (imm == 0) return 0;
            return mk_i(imm, /*x2*/2, 0x0, rd_p, 0x13);
        }
        case 0x2: {
            /* C.LW: lw rd', uimm(rs1')
             *   c[12:10] = uimm[5:3]
             *   c[6]     = uimm[2]
             *   c[5]     = uimm[6] */
            uint32_t rd_p  = creg((c >> 2) & 0x7u);
            uint32_t rs1_p = creg((c >> 7) & 0x7u);
            uint32_t imm =
                (((c >> 10) & 0x7u) << 3) |
                (((c >> 6)  & 0x1u) << 2) |
                (((c >> 5)  & 0x1u) << 6);
            return mk_i(imm, rs1_p, 0x2, rd_p, 0x03);
        }
        case 0x6: {
            /* C.SW: sw rs2', uimm(rs1') — same imm layout as C.LW */
            uint32_t rs2_p = creg((c >> 2) & 0x7u);
            uint32_t rs1_p = creg((c >> 7) & 0x7u);
            uint32_t imm =
                (((c >> 10) & 0x7u) << 3) |
                (((c >> 6)  & 0x1u) << 2) |
                (((c >> 5)  & 0x1u) << 6);
            return mk_s(imm, rs2_p, rs1_p, 0x2, 0x23);
        }
        default:
            return 0;   /* C.FLD, C.LQ, C.FSD, C.SQ etc. — not supported */
        }
    }

    if (quadrant == 1) {
        /* === QUADRANT 1 === */
        switch (f3) {
        case 0x0: {
            /* C.NOP (rd=0, nzimm=0) or C.ADDI rd, nzimm
             *   c[12]   = imm[5] (sign bit)
             *   c[6:2]  = imm[4:0]
             * rd != 0; nzimm = 0 with rd != 0 is a HINT (legal nop). */
            uint32_t rd = (c >> 7) & 0x1Fu;
            uint32_t imm = (((c >> 12) & 0x1u) << 5) | ((c >> 2) & 0x1Fu);
            int32_t simm = sign_extend(imm, 6);
            /* If rd=0 and imm=0, this is C.NOP — expand to addi x0,x0,0
             * which the dispatcher treats as a no-op (writes to x0
             * get squashed). */
            return mk_i((uint32_t)simm, rd, 0x0, rd, 0x13);
        }
        case 0x1: {
            /* C.JAL: jal x1, offset
             *   Imm layout (offset is signed, byte-addressed, bit 0 = 0):
             *     c[12]   = imm[11]  (sign)
             *     c[11]   = imm[4]
             *     c[10:9] = imm[9:8]
             *     c[8]    = imm[10]
             *     c[7]    = imm[6]
             *     c[6]    = imm[7]
             *     c[5:3]  = imm[3:1]
             *     c[2]    = imm[5] */
            uint32_t imm =
                (((c >> 12) & 0x1u) << 11) |
                (((c >> 11) & 0x1u) << 4)  |
                (((c >> 9)  & 0x3u) << 8)  |
                (((c >> 8)  & 0x1u) << 10) |
                (((c >> 7)  & 0x1u) << 6)  |
                (((c >> 6)  & 0x1u) << 7)  |
                (((c >> 3)  & 0x7u) << 1)  |
                (((c >> 2)  & 0x1u) << 5);
            int32_t simm = sign_extend(imm, 12);
            return mk_j((uint32_t)simm, /*x1*/1, 0x6F);
        }
        case 0x2: {
            /* C.LI: addi rd, x0, imm
             *   c[12]   = imm[5] (sign)
             *   c[6:2]  = imm[4:0]
             * rd != 0; rd=0 is a HINT (we still expand it). */
            uint32_t rd = (c >> 7) & 0x1Fu;
            uint32_t imm = (((c >> 12) & 0x1u) << 5) | ((c >> 2) & 0x1Fu);
            int32_t simm = sign_extend(imm, 6);
            return mk_i((uint32_t)simm, /*x0*/0, 0x0, rd, 0x13);
        }
        case 0x3: {
            /* C.ADDI16SP (rd=2) or C.LUI (rd!=2, rd!=0)
             * rd=0 with these encodings is reserved.
             *
             * C.ADDI16SP: addi x2, x2, nzimm
             *   c[12]   = nzimm[9]  (sign)
             *   c[6]    = nzimm[4]
             *   c[5]    = nzimm[6]
             *   c[4:3]  = nzimm[8:7]
             *   c[2]    = nzimm[5]
             *
             * C.LUI: lui rd, nzimm
             *   c[12]   = nzimm[17] (sign)
             *   c[6:2]  = nzimm[16:12]
             *   (low 12 bits of result are zero) */
            uint32_t rd = (c >> 7) & 0x1Fu;
            if (rd == 0) return 0;
            if (rd == 2) {
                uint32_t imm =
                    (((c >> 12) & 0x1u) << 9) |
                    (((c >> 6)  & 0x1u) << 4) |
                    (((c >> 5)  & 0x1u) << 6) |
                    (((c >> 3)  & 0x3u) << 7) |
                    (((c >> 2)  & 0x1u) << 5);
                if (imm == 0) return 0;
                int32_t simm = sign_extend(imm, 10);
                return mk_i((uint32_t)simm, /*x2*/2, 0x0, /*x2*/2, 0x13);
            }
            /* C.LUI */
            uint32_t imm =
                (((c >> 12) & 0x1u) << 17) |
                (((c >> 2)  & 0x1Fu) << 12);
            if (imm == 0) return 0;
            int32_t simm = sign_extend(imm, 18);
            return mk_u((uint32_t)simm, rd, 0x37);
        }
        case 0x4: {
            /* Sub-quadrant on c[11:10]:
             *   00: C.SRLI rd', shamt
             *   01: C.SRAI rd', shamt
             *   10: C.ANDI rd', imm
             *   11: ALU on c[6:5]:
             *        00 C.SUB / 01 C.XOR / 10 C.OR / 11 C.AND */
            uint32_t sub   = (c >> 10) & 0x3u;
            uint32_t rd_p  = creg((c >> 7) & 0x7u);
            if (sub == 0x0 || sub == 0x1) {
                /* Shifts: c[12]=shamt[5] (must be 0 in RV32C),
                 *         c[6:2]=shamt[4:0]
                 * shamt[5] != 0 is reserved in RV32. */
                uint32_t shamt5 = (c >> 12) & 0x1u;
                uint32_t shamt40 = (c >> 2) & 0x1Fu;
                if (shamt5 != 0) return 0;
                /* SRLI: funct7=0x00, funct3=0x5
                 * SRAI: funct7=0x20, funct3=0x5 */
                uint32_t f7 = (sub == 0x1) ? 0x20u : 0x00u;
                return mk_i((f7 << 5) | shamt40,
                            rd_p, 0x5, rd_p, 0x13);
            }
            if (sub == 0x2) {
                /* C.ANDI rd', imm
                 *   c[12]=imm[5] (sign), c[6:2]=imm[4:0] */
                uint32_t imm = (((c >> 12) & 0x1u) << 5) |
                               ((c >> 2) & 0x1Fu);
                int32_t simm = sign_extend(imm, 6);
                return mk_i((uint32_t)simm, rd_p, 0x7, rd_p, 0x13);
            }
            /* sub == 0x3: ALU ops */
            uint32_t rs2_p = creg((c >> 2) & 0x7u);
            uint32_t op    = (c >> 5) & 0x3u;
            /* The c[12] bit selects between RV32C base ops (=0)
             * and would select RV64/wider variants (=1). For RV32
             * c[12]=1 with sub==3 is reserved. */
            if (((c >> 12) & 0x1u) != 0) return 0;
            switch (op) {
            case 0x0: /* C.SUB → sub */
                return mk_r(0x20, rs2_p, rd_p, 0x0, rd_p, 0x33);
            case 0x1: /* C.XOR → xor */
                return mk_r(0x00, rs2_p, rd_p, 0x4, rd_p, 0x33);
            case 0x2: /* C.OR → or */
                return mk_r(0x00, rs2_p, rd_p, 0x6, rd_p, 0x33);
            case 0x3: /* C.AND → and */
                return mk_r(0x00, rs2_p, rd_p, 0x7, rd_p, 0x33);
            }
            return 0;   /* unreachable */
        }
        case 0x5: {
            /* C.J: jal x0, offset — same imm format as C.JAL */
            uint32_t imm =
                (((c >> 12) & 0x1u) << 11) |
                (((c >> 11) & 0x1u) << 4)  |
                (((c >> 9)  & 0x3u) << 8)  |
                (((c >> 8)  & 0x1u) << 10) |
                (((c >> 7)  & 0x1u) << 6)  |
                (((c >> 6)  & 0x1u) << 7)  |
                (((c >> 3)  & 0x7u) << 1)  |
                (((c >> 2)  & 0x1u) << 5);
            int32_t simm = sign_extend(imm, 12);
            return mk_j((uint32_t)simm, /*x0*/0, 0x6F);
        }
        case 0x6:
        case 0x7: {
            /* C.BEQZ / C.BNEZ: branch if rs1' == 0 (or != 0)
             *   c[12]   = imm[8] (sign)
             *   c[11:10]= imm[4:3]
             *   c[6:5]  = imm[7:6]
             *   c[4:3]  = imm[2:1]
             *   c[2]    = imm[5] */
            uint32_t rs1_p = creg((c >> 7) & 0x7u);
            uint32_t imm =
                (((c >> 12) & 0x1u) << 8) |
                (((c >> 10) & 0x3u) << 3) |
                (((c >> 5)  & 0x3u) << 6) |
                (((c >> 3)  & 0x3u) << 1) |
                (((c >> 2)  & 0x1u) << 5);
            int32_t simm = sign_extend(imm, 9);
            uint32_t branch_f3 = (f3 == 0x6) ? 0x0 : 0x1;   /* beq vs bne */
            return mk_b((uint32_t)simm, /*x0*/0, rs1_p, branch_f3, 0x63);
        }
        }
        return 0;
    }

    /* quadrant == 2 */
    {
        /* === QUADRANT 2 === */
        switch (f3) {
        case 0x0: {
            /* C.SLLI: slli rd, rd, shamt
             *   c[12]=shamt[5] (must be 0 in RV32), c[6:2]=shamt[4:0]
             * rd != 0 (rd=0 is a HINT). */
            uint32_t rd = (c >> 7) & 0x1Fu;
            uint32_t shamt5 = (c >> 12) & 0x1u;
            uint32_t shamt40 = (c >> 2) & 0x1Fu;
            if (shamt5 != 0) return 0;
            /* SLLI: funct7=0x00, funct3=0x1 */
            return mk_i(shamt40, rd, 0x1, rd, 0x13);
        }
        case 0x2: {
            /* C.LWSP: lw rd, uimm(x2)
             *   c[12]   = uimm[5]
             *   c[6:4]  = uimm[4:2]
             *   c[3:2]  = uimm[7:6]
             * rd != 0 (rd=0 is reserved). */
            uint32_t rd = (c >> 7) & 0x1Fu;
            if (rd == 0) return 0;
            uint32_t imm =
                (((c >> 12) & 0x1u) << 5) |
                (((c >> 4)  & 0x7u) << 2) |
                (((c >> 2)  & 0x3u) << 6);
            return mk_i(imm, /*x2*/2, 0x2, rd, 0x03);
        }
        case 0x4: {
            /* Four sub-cases based on c[12] and whether rs2 (c[6:2]) is 0.
             *   c[12]=0, rs2=0:  C.JR rs1            → jalr x0, 0(rs1)
             *   c[12]=0, rs2!=0: C.MV rd, rs2        → add rd, x0, rs2
             *   c[12]=1, rs1=rs2=0: C.EBREAK         → ebreak
             *   c[12]=1, rs1!=0, rs2=0: C.JALR rs1   → jalr x1, 0(rs1)
             *   c[12]=1, rs1!=0, rs2!=0: C.ADD       → add rd, rd, rs2 */
            uint32_t rd_or_rs1 = (c >> 7) & 0x1Fu;
            uint32_t rs2       = (c >> 2) & 0x1Fu;
            uint32_t bit12     = (c >> 12) & 0x1u;

            if (bit12 == 0) {
                if (rs2 == 0) {
                    /* C.JR: rs1 != 0; rs1=0 is reserved */
                    if (rd_or_rs1 == 0) return 0;
                    return mk_i(0, rd_or_rs1, 0x0, /*x0*/0, 0x67);
                }
                /* C.MV: add rd, x0, rs2 */
                return mk_r(0x00, rs2, /*x0*/0, 0x0, rd_or_rs1, 0x33);
            }
            /* bit12 == 1 */
            if (rd_or_rs1 == 0 && rs2 == 0) {
                /* C.EBREAK */
                return 0x00100073u;
            }
            if (rs2 == 0) {
                /* C.JALR: jalr x1, 0(rs1) */
                return mk_i(0, rd_or_rs1, 0x0, /*x1*/1, 0x67);
            }
            /* C.ADD: add rd, rd, rs2; rd != 0 (rd=0 is reserved here) */
            if (rd_or_rs1 == 0) return 0;
            return mk_r(0x00, rs2, rd_or_rs1, 0x0, rd_or_rs1, 0x33);
        }
        case 0x6: {
            /* C.SWSP: sw rs2, uimm(x2)
             *   c[12:9] = uimm[5:2]
             *   c[8:7]  = uimm[7:6] */
            uint32_t rs2 = (c >> 2) & 0x1Fu;
            uint32_t imm =
                (((c >> 9) & 0xFu) << 2) |
                (((c >> 7) & 0x3u) << 6);
            return mk_s(imm, rs2, /*x2*/2, 0x2, 0x23);
        }
        default:
            return 0;
        }
    }
}

/* ============================================================
 *  Instruction execution
 *
 *  Outer switch on the 7-bit major opcode. Nested switches on
 *  funct3 / funct7 where needed. Each handler:
 *    - reads operands from cpu->regs / instruction bits
 *    - computes the result
 *    - writes cpu->regs[rd] (writes to x0 are squashed by step_once)
 *    - advances cpu->pc by insn_size
 *
 *  Returns true on success, false on trap (with trap state set).
 * ============================================================ */

static bool execute_one(VmCpu *cpu, uint32_t insn, uint32_t insn_size) {
    /* Compressed instructions are expanded in place before the
     * main decode. After this block, insn is always the 32-bit
     * encoding; insn_size remains 2 so PC advances by 2 bytes. */
    if (insn_size == 2) {
        uint32_t expanded = expand_compressed((uint16_t)insn);
        if (expanded == 0) {
            cpu->trap_cause = TRAP_ILLEGAL_INSTR;
            cpu->trap_pc    = cpu->pc;
            cpu->trap_insn  = insn;
            return false;
        }
        insn = expanded;
    }

    uint32_t op  = opcode_of(insn);
    uint32_t rd  = rd_of(insn);
    uint32_t rs1 = rs1_of(insn);
    uint32_t rs2 = rs2_of(insn);
    uint32_t f3  = funct3_of(insn);
    uint32_t f7  = funct7_of(insn);

    switch (op) {

    /* ============================================================
     *  OP-IMM (0x13) — register-immediate ALU
     * ============================================================ */
    case 0x13: {
        uint32_t a = cpu->regs[rs1];
        int32_t  imm = imm_i(insn);
        uint32_t uimm = (uint32_t)imm;
        uint32_t result = 0;

        switch (f3) {
        case 0x0: /* ADDI */
            result = a + uimm;
            break;
        case 0x2: /* SLTI (signed compare) */
            result = ((int32_t)a < imm) ? 1u : 0u;
            break;
        case 0x3: /* SLTIU (unsigned compare); SLTIU rd, rs1, 1
                   * is the canonical "set if zero" idiom */
            result = (a < uimm) ? 1u : 0u;
            break;
        case 0x4: /* XORI */
            result = a ^ uimm;
            break;
        case 0x6: /* ORI */
            result = a | uimm;
            break;
        case 0x7: /* ANDI */
            result = a & uimm;
            break;
        case 0x1: /* SLLI — funct7 must be 0 */
            if (f7 != 0x00) goto illegal;
            result = a << shamt_of(insn);
            break;
        case 0x5: /* SRLI / SRAI — funct7 distinguishes */
            if (f7 == 0x00) {
                result = a >> shamt_of(insn);     /* SRLI: logical */
            } else if (f7 == 0x20) {
                result = sra32(a, shamt_of(insn)); /* SRAI: arithmetic */
            } else {
                goto illegal;
            }
            break;
        default:
            goto illegal;
        }

        cpu->regs[rd] = result;
        cpu->pc += insn_size;
        return true;
    }

    /* ============================================================
     *  OP (0x33) — register-register ALU
     *
     *  funct7 selects between the base I extension (0x00, 0x20)
     *  and the M extension (0x01). M is checked first so the
     *  existing I-extension switch below stays intact.
     * ============================================================ */
    case 0x33: {
        uint32_t a = cpu->regs[rs1];
        uint32_t b = cpu->regs[rs2];
        uint32_t result = 0;

        /* ====== RV32M ====== */
        if (f7 == 0x01) {
            switch (f3) {
            case 0x0: /* MUL — low 32 bits of 32x32 product */
                result = a * b;
                break;
            case 0x1: { /* MULH — high 32 bits, signed × signed */
                int64_t prod = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
                result = (uint32_t)((uint64_t)prod >> 32);
                break;
            }
            case 0x2: { /* MULHSU — high 32 bits, signed × unsigned */
                int64_t prod = (int64_t)(int32_t)a * (int64_t)(uint32_t)b;
                result = (uint32_t)((uint64_t)prod >> 32);
                break;
            }
            case 0x3: { /* MULHU — high 32 bits, unsigned × unsigned */
                uint64_t prod = (uint64_t)a * (uint64_t)b;
                result = (uint32_t)(prod >> 32);
                break;
            }
            case 0x4: { /* DIV — signed division, truncated toward zero
                         *
                         * Two spec-mandated special cases:
                         *   divisor = 0:               result = -1
                         *   INT32_MIN / -1 overflow:   result = INT32_MIN
                         *
                         * Both are required to NOT trap. C's signed
                         * division would be undefined for the overflow
                         * case and trap for divide-by-zero on many
                         * hosts, so we must guard explicitly. */
                if (b == 0) {
                    result = 0xFFFFFFFFu;
                } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
                    result = 0x80000000u;
                } else {
                    result = (uint32_t)((int32_t)a / (int32_t)b);
                }
                break;
            }
            case 0x5: /* DIVU — unsigned division
                       *   divisor = 0: result = all ones */
                if (b == 0) {
                    result = 0xFFFFFFFFu;
                } else {
                    result = a / b;
                }
                break;
            case 0x6: { /* REM — signed remainder, sign follows dividend
                         *
                         * Specials:
                         *   divisor = 0:               result = dividend
                         *   INT32_MIN % -1 overflow:   result = 0 */
                if (b == 0) {
                    result = a;
                } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
                    result = 0;
                } else {
                    result = (uint32_t)((int32_t)a % (int32_t)b);
                }
                break;
            }
            case 0x7: /* REMU — unsigned remainder
                       *   divisor = 0: result = dividend */
                if (b == 0) {
                    result = a;
                } else {
                    result = a % b;
                }
                break;
            default:
                goto illegal;
            }
            cpu->regs[rd] = result;
            cpu->pc += insn_size;
            return true;
        }

        /* ====== RV32I (existing) ====== */
        switch (f3) {
        case 0x0: /* ADD / SUB */
            if (f7 == 0x00)      result = a + b;       /* ADD */
            else if (f7 == 0x20) result = a - b;       /* SUB */
            else goto illegal;
            break;
        case 0x1: /* SLL */
            if (f7 != 0x00) goto illegal;
            result = a << (b & 0x1Fu);
            break;
        case 0x2: /* SLT (signed) */
            if (f7 != 0x00) goto illegal;
            result = ((int32_t)a < (int32_t)b) ? 1u : 0u;
            break;
        case 0x3: /* SLTU (unsigned) */
            if (f7 != 0x00) goto illegal;
            result = (a < b) ? 1u : 0u;
            break;
        case 0x4: /* XOR */
            if (f7 != 0x00) goto illegal;
            result = a ^ b;
            break;
        case 0x5: /* SRL / SRA */
            if (f7 == 0x00)      result = a >> (b & 0x1Fu);
            else if (f7 == 0x20) result = sra32(a, b & 0x1Fu);
            else goto illegal;
            break;
        case 0x6: /* OR */
            if (f7 != 0x00) goto illegal;
            result = a | b;
            break;
        case 0x7: /* AND */
            if (f7 != 0x00) goto illegal;
            result = a & b;
            break;
        default:
            goto illegal;
        }

        cpu->regs[rd] = result;
        cpu->pc += insn_size;
        return true;
    }

    /* ============================================================
     *  LUI (0x37) — load upper immediate
     *  AUIPC (0x17) — add upper immediate to PC
     * ============================================================ */
    case 0x37: /* LUI */
        cpu->regs[rd] = (uint32_t)imm_u(insn);
        cpu->pc += insn_size;
        return true;

    case 0x17: /* AUIPC */
        cpu->regs[rd] = cpu->pc + (uint32_t)imm_u(insn);
        cpu->pc += insn_size;
        return true;

    /* ============================================================
     *  LOAD (0x03) — LB/LH/LW/LBU/LHU
     *
     *  Address = rs1 + imm_i. The width and signedness of the
     *  result depends on funct3.
     * ============================================================ */
    case 0x03: {
        uint32_t addr = cpu->regs[rs1] + (uint32_t)imm_i(insn);
        uint32_t result = 0;

        switch (f3) {
        case 0x0: { /* LB: 8-bit, sign-extended */
            const uint8_t *p = xlat_read(cpu, addr, 1, TRAP_LOAD_FAULT);
            if (!p) return false;
            result = (uint32_t)(int32_t)(int8_t)*p;
            break;
        }
        case 0x1: { /* LH: 16-bit, sign-extended; needs 2-byte align */
            if (addr & 0x1u) {
                cpu->trap_cause = TRAP_LOAD_MISALIGNED;
                cpu->trap_pc    = cpu->pc;
                cpu->trap_addr  = addr;
                return false;
            }
            const uint8_t *p = xlat_read(cpu, addr, 2, TRAP_LOAD_FAULT);
            if (!p) return false;
            uint16_t v;
            memcpy(&v, p, 2);
            result = (uint32_t)(int32_t)(int16_t)v;
            break;
        }
        case 0x2: { /* LW: 32-bit; needs 4-byte align */
            if (addr & 0x3u) {
                cpu->trap_cause = TRAP_LOAD_MISALIGNED;
                cpu->trap_pc    = cpu->pc;
                cpu->trap_addr  = addr;
                return false;
            }
            const uint8_t *p = xlat_read(cpu, addr, 4, TRAP_LOAD_FAULT);
            if (!p) return false;
            memcpy(&result, p, 4);
            break;
        }
        case 0x4: { /* LBU: 8-bit, zero-extended */
            const uint8_t *p = xlat_read(cpu, addr, 1, TRAP_LOAD_FAULT);
            if (!p) return false;
            result = (uint32_t)*p;
            break;
        }
        case 0x5: { /* LHU: 16-bit, zero-extended; needs 2-byte align */
            if (addr & 0x1u) {
                cpu->trap_cause = TRAP_LOAD_MISALIGNED;
                cpu->trap_pc    = cpu->pc;
                cpu->trap_addr  = addr;
                return false;
            }
            const uint8_t *p = xlat_read(cpu, addr, 2, TRAP_LOAD_FAULT);
            if (!p) return false;
            uint16_t v;
            memcpy(&v, p, 2);
            result = (uint32_t)v;
            break;
        }
        default:
            goto illegal;
        }

        cpu->regs[rd] = result;
        cpu->pc += insn_size;
        return true;
    }

    /* ============================================================
     *  STORE (0x23) — SB/SH/SW
     *
     *  Address = rs1 + imm_s. Value to store is the low N bits
     *  of rs2.
     * ============================================================ */
    case 0x23: {
        uint32_t addr = cpu->regs[rs1] + (uint32_t)imm_s(insn);
        uint32_t value = cpu->regs[rs2];

        switch (f3) {
        case 0x0: { /* SB: low 8 bits */
            uint8_t *p = xlat_write(cpu, addr, 1);
            if (!p) return false;
            *p = (uint8_t)(value & 0xFFu);
            break;
        }
        case 0x1: { /* SH: low 16 bits; needs 2-byte align */
            if (addr & 0x1u) {
                cpu->trap_cause = TRAP_STORE_MISALIGNED;
                cpu->trap_pc    = cpu->pc;
                cpu->trap_addr  = addr;
                return false;
            }
            uint8_t *p = xlat_write(cpu, addr, 2);
            if (!p) return false;
            uint16_t v = (uint16_t)(value & 0xFFFFu);
            memcpy(p, &v, 2);
            break;
        }
        case 0x2: { /* SW: full 32 bits; needs 4-byte align */
            if (addr & 0x3u) {
                cpu->trap_cause = TRAP_STORE_MISALIGNED;
                cpu->trap_pc    = cpu->pc;
                cpu->trap_addr  = addr;
                return false;
            }
            uint8_t *p = xlat_write(cpu, addr, 4);
            if (!p) return false;
            memcpy(p, &value, 4);
            break;
        }
        default:
            goto illegal;
        }

        cpu->pc += insn_size;
        return true;
    }

    /* ============================================================
     *  BRANCH (0x63) — BEQ/BNE/BLT/BGE/BLTU/BGEU
     *
     *  If condition true: pc += imm_b. Else: pc += insn_size.
     *  Branch targets are 2-byte aligned by encoding (imm[0]=0).
     * ============================================================ */
    case 0x63: {
        uint32_t a = cpu->regs[rs1];
        uint32_t b = cpu->regs[rs2];
        bool take = false;

        switch (f3) {
        case 0x0: take = (a == b);                                 break; /* BEQ  */
        case 0x1: take = (a != b);                                 break; /* BNE  */
        case 0x4: take = ((int32_t)a <  (int32_t)b);               break; /* BLT  */
        case 0x5: take = ((int32_t)a >= (int32_t)b);               break; /* BGE  */
        case 0x6: take = (a <  b);                                 break; /* BLTU */
        case 0x7: take = (a >= b);                                 break; /* BGEU */
        default:  goto illegal;
        }

        if (take) {
            cpu->pc = cpu->pc + (uint32_t)imm_b(insn);
        } else {
            cpu->pc += insn_size;
        }
        return true;
    }

    /* ============================================================
     *  JAL (0x6F) — jump and link
     *
     *  rd = pc + insn_size (return address)
     *  pc = pc + imm_j     (target)
     *
     *  Targets are 2-byte aligned by encoding.
     * ============================================================ */
    case 0x6F: {
        uint32_t link = cpu->pc + insn_size;
        uint32_t target = cpu->pc + (uint32_t)imm_j(insn);
        cpu->regs[rd] = link;
        cpu->pc = target;
        return true;
    }

    /* ============================================================
     *  JALR (0x67) — jump and link register
     *
     *  rd = pc + insn_size
     *  pc = (rs1 + imm_i) & ~1  (low bit cleared per spec)
     *
     *  Reading rs1 BEFORE writing rd matters when rd == rs1.
     * ============================================================ */
    case 0x67: {
        if (f3 != 0x0) goto illegal;
        uint32_t link = cpu->pc + insn_size;
        uint32_t target = (cpu->regs[rs1] + (uint32_t)imm_i(insn)) & ~1u;
        cpu->regs[rd] = link;
        cpu->pc = target;
        return true;
    }

    /* ============================================================
     *  MISC-MEM (0x0F) — FENCE, FENCE.I
     *
     *  Single-hart VM with no real memory model: both are nops.
     *  The bit fields (pred/succ for FENCE) are accepted as hints
     *  but not validated.
     * ============================================================ */
    case 0x0F:
        if (f3 != 0x0 && f3 != 0x1) goto illegal;
        cpu->pc += insn_size;
        return true;

    /* ============================================================
     *  SYSTEM (0x73) — ECALL, EBREAK, CSR*
     * ============================================================ */
    case 0x73: {
        if (f3 == 0x0) {
            /* ECALL / EBREAK distinguished by the imm field. */
            uint32_t imm = (insn >> 20) & 0xFFFu;

            /* Other bits of insn beyond rs1 and rd must be 0 too;
             * we don't strictly enforce, matching liberal-decode
             * spirit. */
            if (imm == 0x000) {
                /* ECALL — advance PC past the instruction so the
                 * host's ECALL handler returns to the instruction
                 * AFTER the ecall, then trap with ECALL cause. */
                cpu->pc += insn_size;
                cpu->trap_cause = TRAP_ECALL;
                cpu->trap_pc    = cpu->pc - insn_size;
                return false;
            }
            if (imm == 0x001) {
                /* EBREAK — do NOT advance PC. The debugger needs
                 * PC to point at the ebreak so it can replace it
                 * with the original instruction and re-execute. */
                cpu->trap_cause = TRAP_BREAKPOINT;
                cpu->trap_pc    = cpu->pc;
                cpu->trap_insn  = insn;
                return false;
            }
            goto illegal;
        }

        /* CSR access. funct3 selects op + immediate-vs-register.
         *
         * CSRRW/RS/RC use rs1 as the value source.
         * CSRRWI/RSI/RCI use the rs1 field as a 5-bit unsigned
         * immediate.
         *
         * Implementation order: read old value, compute new value,
         * write rd FIRST, then update CSR. (The read-then-write
         * order matters in theory; in our shim where writes are
         * no-ops it doesn't, but doing it the spec way keeps the
         * code honest if we ever implement a writable CSR.) */
        uint32_t csr = (insn >> 20) & 0xFFFu;
        uint32_t source;
        bool use_imm = (f3 & 0x4u) != 0;
        if (use_imm) {
            source = rs1;   /* the rs1 field IS the immediate */
        } else {
            source = cpu->regs[rs1];
        }

        uint32_t old_value = csr_read(cpu, csr);
        uint32_t new_value = old_value;
        bool do_write = true;

        switch (f3 & 0x3u) {
        case 0x1: /* CSRRW / CSRRWI: replace */
            new_value = source;
            break;
        case 0x2: /* CSRRS / CSRRSI: set bits */
            new_value = old_value | source;
            /* Spec: CSRRS with rs1=x0 (or CSRRSI with uimm=0) does
             * NOT write the CSR. Important for CSRs with side
             * effects on write — none of ours, but be faithful. */
            if (source == 0) do_write = false;
            break;
        case 0x3: /* CSRRC / CSRRCI: clear bits */
            new_value = old_value & ~source;
            if (source == 0) do_write = false;
            break;
        default:
            goto illegal;   /* funct3 == 0x4 — invalid */
        }

        /* Write rd first (in case rd == something that csr_write
         * might affect — it doesn't today, but be safe). */
        cpu->regs[rd] = old_value;
        if (do_write) csr_write(cpu, csr, new_value);

        cpu->pc += insn_size;
        return true;
    }

    default:
        goto illegal;
    }

illegal:
    cpu->trap_cause = TRAP_ILLEGAL_INSTR;
    cpu->trap_pc    = cpu->pc;
    cpu->trap_insn  = insn;
    return false;
}

/* ============================================================
 *  step_once — one iteration of the dispatch loop, factored out
 *
 *  Returns:
 *    true if the instruction was retired normally and we should
 *         continue iterating (PC advanced, regs updated)
 *    false if we need to exit the loop (trap of any kind set in
 *         cpu->trap_cause; caller routes by inspecting it)
 *
 *  Marked static inline so the dispatch loop is one compilation
 *  unit and the compiler can fold the fetch+execute pair into the
 *  caller. This also makes future unrolling a small local change:
 *  the unrolled loop is just `step_once(cpu)` repeated N times
 *  with break-on-false checks between each.
 * ============================================================ */

static inline bool step_once(VmCpu *cpu) {
    uint32_t insn = 0;
    uint32_t insn_size = 0;

    if (!fetch_instruction(cpu, &insn, &insn_size)) {
        cpu->trap_count++;
        return false;
    }

    if (!execute_one(cpu, insn, insn_size)) {
        /* ECALL is a routine exit, not an error — don't bump
         * trap_count for it. The dispatcher (vm_step) bumps
         * ecall_count separately when routing it. */
        if (cpu->trap_cause != TRAP_ECALL) {
            cpu->trap_count++;
        }
        return false;
    }

    /* x0 is hardwired to zero. Squash any write to regs[0]. */
    cpu->regs[0] = 0;
    cpu->instructions_retired++;
    return true;
}

/* ============================================================
 *  vm_step — main dispatch loop
 * ============================================================ */

VmStepResult vm_step(VmCpu *cpu, uint32_t budget, uint32_t *out_steps) {
    uint32_t steps = 0;

    if (!cpu) {
        if (out_steps) *out_steps = 0;
        return VM_STEP_HALTED;
    }

    if (cpu->halted) {
        if (out_steps) *out_steps = 0;
        return VM_STEP_HALTED;
    }

    /* Clear trap state at the start of every step call. Any
     * trap that occurred on a previous call has been reported;
     * resuming means a clean slate. */
    cpu->trap_cause = TRAP_NONE;
    cpu->trap_pc    = 0;
    cpu->trap_addr  = 0;
    cpu->trap_insn  = 0;

    while (steps < budget) {
        if (!step_once(cpu)) {
            if (out_steps) *out_steps = steps;

            switch (cpu->trap_cause) {
                case TRAP_ECALL:
                    cpu->ecall_count++;
                    return VM_STEP_ECALL;
                case TRAP_HALT:
                    cpu->halted = true;
                    return VM_STEP_HALTED;
                default:
                    return VM_STEP_TRAPPED;
            }
        }
        steps++;
    }

    if (out_steps) *out_steps = steps;
    return VM_STEP_QUANTUM_EXPIRED;
}
