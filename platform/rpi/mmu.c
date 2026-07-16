/* platform/rpi/mmu.c — ARM1176 (ARMv6) flat identity map + L1 caches.
 *
 * The Pi Zero's ARM1176 runs UNCACHED with the MMU off, which crawls.
 * Enabling the L1 I/D caches on ARMv6 requires the MMU on with a page
 * table present (cacheability is a per-section attribute). This is NOT
 * virtual memory: it's a 1:1 (VA==PA) map whose only job is to attach
 * memory attributes — normal write-back/write-allocate for RAM so it
 * caches, device for the peripheral window so MMIO stays coherent.
 *
 * 4096 first-level section descriptors (1 MB each) cover the full 4 GB.
 * BCM2835: RAM at 0x0000_0000.. ; peripherals at 0x2000_0000-0x20FF_FFFF.
 *
 * QEMU (functional) enforces translation + attributes but not cache
 * timing, so this verifies correctness (no faults) here; the speedup is
 * a real-hardware property. See docs/rpi-port.md.
 *
 * Public domain (CC0). No warranty.
 */

#include <stdint.h>

#define NUM_SECTIONS   4096u
#define RAM_SECTIONS    512u    /* 0x0000_0000-0x1FFF_FFFF = 512 MB of RAM */

/* First-level translation table: 4096 words, 16 KB, 16 KB-aligned (TTBR0
 * requires bits[13:0]==0). Lives in BSS. */
static uint32_t __attribute__((aligned(16384))) l1_table[NUM_SECTIONS];

/* ---- ARMv6 section descriptor fields (XP=1 / subpages-disabled form) ----
 *   [1:0]=0b10 section, [2]=B, [3]=C, [4]=XN, [8:5]=Domain, [11:10]=AP,
 *   [14:12]=TEX, [15]=APX, [16]=S, [17]=nG, [31:20]=base.
 * Memory type (non-remap ARMv6):
 *   Normal WB-WA (cacheable RAM): TEX=001, C=1, B=1
 *   Shareable Device (MMIO):      TEX=000, C=0, B=1  + XN                 */
#define SEC_SECTION   0x2u
#define SEC_AP_RW     (0x3u << 10)      /* AP=0b11 full access, APX=0     */
#define SEC_DOMAIN0   (0x0u << 5)
#define SEC_XN        (0x1u << 4)
#define SEC_MEM_RAM   ((0x1u << 12) | (1u << 3) | (1u << 2))   /* TEX=001,C,B */
#define SEC_MEM_DEV   ((0x0u << 12) | (0u << 3) | (1u << 2))   /* TEX=000,B  */

void mmu_enable(void) {
    for (uint32_t i = 0; i < NUM_SECTIONS; i++) {
        uint32_t base = i << 20;
        uint32_t attr;
        if (i < RAM_SECTIONS) {
            attr = SEC_MEM_RAM | SEC_AP_RW | SEC_DOMAIN0;              /* cacheable RAM */
        } else {
            attr = SEC_MEM_DEV | SEC_AP_RW | SEC_DOMAIN0 | SEC_XN;     /* device (MMIO + rest) */
        }
        l1_table[i] = base | attr | SEC_SECTION;
    }

    uint32_t ttbr = (uint32_t)(uintptr_t)l1_table;

    __asm__ volatile(
        "mov    r0, #1                \n" /* DACR: domain 0 = client (respect AP) */
        "mcr    p15, 0, r0, c3, c0, 0 \n"
        "mcr    p15, 0, %0, c2, c0, 0 \n" /* TTBR0 = table base                   */
        "mov    r0, #0                \n"
        "mcr    p15, 0, r0, c2, c0, 2 \n" /* TTBCR = 0 (TTBR0 covers all)          */
        "mcr    p15, 0, r0, c8, c7, 0 \n" /* invalidate unified TLB                */
        "mcr    p15, 0, r0, c7, c7, 0 \n" /* invalidate I + D caches               */
        "mcr    p15, 0, r0, c7, c10, 4\n" /* DSB                                   */
        "mcr    p15, 0, r0, c7, c5, 4 \n" /* ISB (prefetch flush)                  */
        "mrc    p15, 0, r0, c1, c0, 0 \n" /* read SCTLR                            */
        "orr    r0, r0, #0x00000001   \n" /* M  : MMU enable                       */
        "orr    r0, r0, #0x00000004   \n" /* C  : D-cache enable                   */
        "orr    r0, r0, #0x00000800   \n" /* Z  : branch prediction (bit 11)       */
        "orr    r0, r0, #0x00001000   \n" /* I  : I-cache enable (bit 12)          */
        "orr    r0, r0, #0x00800000   \n" /* XP : ARMv6 page-table format (bit 23) */
        "mcr    p15, 0, r0, c1, c0, 0 \n" /* write SCTLR — MMU + caches now on     */
        "mcr    p15, 0, r0, c7, c5, 4 \n" /* ISB                                   */
        :
        : "r"(ttbr)
        : "r0", "memory"
    );
}

/* Read SCTLR (CP15 c1) so callers can prove the MMU/caches are on. */
uint32_t mmu_read_sctlr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}
