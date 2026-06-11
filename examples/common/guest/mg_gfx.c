/* ============================================================
 *  mg_gfx.c — guest-side CHR/palette ecall stubs.
 *  See mg_gfx.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_gfx.h"
#include "vm_runtime.h"

/* SYS_MG_PALETTE_WRITE format flag values. The host extracts the
 * format byte from the 4th argument and dispatches. */
#define MG_PAL_FMT_BGR555 0
#define MG_PAL_FMT_RGB24  1

/* SYS_MG_PALETTE_SNAP_RESTORE op flag values. */
#define MG_PAL_OP_SNAPSHOT 0
#define MG_PAL_OP_RESTORE  1

MgResult mg_chr_upload(uint16_t vram_word, const void *src, uint16_t bytes) {
    return (MgResult)(int32_t)_vm_sys3(SYS_MG_CHR_UPLOAD,
                                       vram_word, (uint32_t)src, bytes);
}

MgResult mg_chr_upload_transient(uint16_t vram_word, const void *src,
                                  uint16_t bytes) {
    return (MgResult)(int32_t)_vm_sys3(SYS_MG_CHR_UPLOAD_TRANSIENT,
                                       vram_word, (uint32_t)src, bytes);
}

void mg_ppu_clean_slate(void) {
    (void)_vm_sys0(SYS_MG_PPU_CLEAN_SLATE);
}

void mg_palette_set(uint8_t idx, uint16_t bgr555) {
    /* Single-color write as a load of length 1. The host knows how
     * to write one entry; saves us a dedicated ecall slot. */
    (void)_vm_sys4(SYS_MG_PALETTE_WRITE,
                   idx, 1, (uint32_t)&bgr555, MG_PAL_FMT_BGR555);
}

void mg_palette_set_rgb(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    uint16_t w = mg_bgr555(r, g, b);
    mg_palette_set(idx, w);
}

void mg_palette_load(uint8_t start, const uint16_t *bgr555_src, uint16_t count) {
    (void)_vm_sys4(SYS_MG_PALETTE_WRITE,
                   start, count, (uint32_t)bgr555_src, MG_PAL_FMT_BGR555);
}

void mg_palette_load_rgb24(uint8_t start, const uint8_t *rgb24_src, uint16_t count) {
    (void)_vm_sys4(SYS_MG_PALETTE_WRITE,
                   start, count, (uint32_t)rgb24_src, MG_PAL_FMT_RGB24);
}

MgResult mg_palette_snapshot(uint16_t *out_512_bytes) {
    return (MgResult)(int32_t)_vm_sys2(SYS_MG_PALETTE_SNAP_RESTORE,
                                       MG_PAL_OP_SNAPSHOT,
                                       (uint32_t)out_512_bytes);
}

MgResult mg_palette_restore(const uint16_t *in_512_bytes) {
    return (MgResult)(int32_t)_vm_sys2(SYS_MG_PALETTE_SNAP_RESTORE,
                                       MG_PAL_OP_RESTORE,
                                       (uint32_t)in_512_bytes);
}

MgResult mg_pack_chr_4bpp(void *dst, const void *src_linear, uint16_t tile_count) {
    return (MgResult)(int32_t)_vm_sys4(SYS_MG_PACK_CHR,
                                       (uint32_t)dst,
                                       (uint32_t)src_linear,
                                       tile_count, 4 /* bpp */);
}

MgResult mg_pack_chr_8bpp(void *dst, const void *src_linear, uint16_t tile_count) {
    return (MgResult)(int32_t)_vm_sys4(SYS_MG_PACK_CHR,
                                       (uint32_t)dst,
                                       (uint32_t)src_linear,
                                       tile_count, 8 /* bpp */);
}
