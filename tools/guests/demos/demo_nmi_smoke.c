/* ============================================================
 *  demo_nmi_smoke.c — Phase 2 install-path bisect smoke test.
 *
 *  v2.26: simplified from the original three-stage RED→GREEN→GREEN
 *  test. The new sequence isolates "is install reaching the kernel"
 *  from "is my dma_list_walk emit correct" by installing a TRIVIAL
 *  handler that just writes INIDISP = $00 (force blank, brightness
 *  0). If installed, the screen turns pure black instead of red.
 *
 *  Stages:
 *
 *    Stage A (seconds 0-2):  backdrop RED, kernel default NMI
 *                            -> screen RED (proves baseline rendering
 *                               + clean_slate cleared the previous
 *                               demo's VRAM).
 *
 *    Stage B (seconds 2-end): install a handler that just sets
 *                            INIDISP=$00 + RTIs. The kernel's @loop
 *                            poll should copy this to WRAM $0E00 and
 *                            rewrite RAMVEC_NMI to point at it. The
 *                            very next NMI runs our trivial handler
 *                            -> screen turns BLACK.
 *
 *  Expected outcome: RED for 2 seconds, then BLACK forever. If you
 *  see RED → RED (no transition), the install path didn't reach
 *  the kernel (likely a stale snes_boot.bin not rebuilt for v2.21+
 *  kernel changes, or h_nmi_install isn't bumping the version). If
 *  you see RED → black flash → RED, the handler is being installed
 *  but RTIing immediately to the default proc somehow.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"
#include "mg_nmi.h"
#include "mg_panic.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

static void wait_n_frames(int n) {
    for (int i = 0; i < n; ++i) {
        mg_frame_commit();
        mg_wait_frame();
    }
}

void _start(void) {
    /* v2.24: clean_slate so previous demo's VRAM/CGRAM doesn't bleed
     * through (without this, running audio_mixer → nmi_smoke shows
     * audio_mixer's text CHR ghosting through nmi_smoke's red bg). */
    mg_ppu_clean_slate();

    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_2, 0x0400, MG_BG_SIZE_32x32, 0x2000);
    mg_bg_enable(MG_BG_LAYER_2, /*main=*/true, /*sub=*/false);

    /* ----- Stage A: RED for 2 seconds (~120 frames) ----- */
    mg_palette_set_rgb(0, 255, 0, 0);
    wait_n_frames(120);

    /* ----- Stage B: install minimal handler — write INIDISP=$00.
     *
     * Handler shape (24 bytes):
     *   prologue       11 B   rep#$30; pha; phx; phy; sep#$20; lda RDNMI
     *   inidisp $00     5 B   lda #$00; sta $2100
     *   out_label       (no opcode — just patches the unused gate jmp)
     *   epilogue        6 B   rep#$30; ply; plx; pla; rti
     *
     * No frame_ready gate — the handler unconditionally force-blanks
     * every NMI. No dma_list_walk — irrelevant for this test. Total
     * 22 bytes. */
    {
        MgNmi b;
        mg_nmi_begin(&b);
        mg_nmi_emit_prologue(&b);
        mg_nmi_emit_inidisp(&b, 0x00);       /* force blank, brightness 0 */
        mg_nmi_emit_out_label(&b);
        mg_nmi_emit_epilogue(&b);
        if (mg_nmi_finish(&b, /*force_blank_lines=*/0) != MG_NMI_OK) {
            mg_panic("nmi smoke: finish failed");
        }
        if (mg_nmi_install(&b) != MG_NMI_OK) {
            mg_panic("nmi smoke: install failed");
        }
    }

    /* Stay forever — the screen should now stay black, proving the
     * install reached the kernel and our trivial handler is running
     * every NMI. */
    for (;;) {
        mg_frame_commit();
        mg_wait_frame();
    }
    (void)sys_exit;
}
