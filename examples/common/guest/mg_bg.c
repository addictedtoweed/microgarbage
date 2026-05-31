/* ============================================================
 *  mg_bg.c — guest-side BG ecall stubs.
 *  See mg_bg.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_bg.h"
#include "vm_runtime.h"

void mg_bg_mode(MgBgMode mode) {
    (void)_vm_sys1(SYS_MG_BG_MODE, (uint32_t)mode);
}

void mg_bg_setup(MgBgLayer layer, uint16_t tilemap_word,
                 MgBgSize size, uint16_t chr_word) {
    (void)_vm_sys4(SYS_MG_BG_SETUP, (uint32_t)layer,
                   tilemap_word, (uint32_t)size, chr_word);
}

void mg_bg_enable(MgBgLayer layer, bool main_screen, bool sub_screen) {
    /* Pack the two screen-enable bits into a single arg. */
    uint32_t mask = (main_screen ? 1u : 0u) | (sub_screen ? 2u : 0u);
    (void)_vm_sys2(SYS_MG_BG_ENABLE, (uint32_t)layer, mask);
}

void mg_bg_set_tile(MgBgLayer layer, uint8_t x, uint8_t y, MgBgTile cell) {
    (void)_vm_sys4(SYS_MG_BG_SET_TILE,
                   (uint32_t)layer, x, y, cell.word);
}

void mg_bg_get_tile(MgBgLayer layer, uint8_t x, uint8_t y, MgBgTile *out) {
    /* Host writes back through the pointer. */
    (void)_vm_sys4(SYS_MG_BG_GET_TILE,
                   (uint32_t)layer, x, y, (uint32_t)out);
}

void mg_bg_blit(MgBgLayer layer, uint8_t x, uint8_t y,
                const MgBgTile *cells, uint16_t n) {
    /* Pack (x,y) into one arg, leaving room for layer + ptr + n. */
    uint32_t xy = ((uint32_t)y << 8) | x;
    (void)_vm_sys4(SYS_MG_BG_BLIT,
                   (uint32_t)layer, xy, (uint32_t)cells, n);
}

MgResult mg_bg_upload(MgBgLayer layer, uint8_t x, uint8_t y,
                      const MgBgTile *cells, uint16_t n) {
    uint32_t xy = ((uint32_t)y << 8) | x;
    return (MgResult)(int32_t)_vm_sys4(SYS_MG_BG_UPLOAD,
                                       (uint32_t)layer, xy,
                                       (uint32_t)cells, n);
}

void mg_bg_scroll(MgBgLayer layer, int16_t hx, int16_t vy) {
    /* int16 sign-extends naturally to uint32 in the calling
     * convention; the host knows to truncate. */
    (void)_vm_sys3(SYS_MG_BG_SCROLL, (uint32_t)layer,
                   (uint32_t)(uint16_t)hx, (uint32_t)(uint16_t)vy);
}

void mg_bg_mosaic(uint8_t size, uint8_t layer_mask) {
    (void)_vm_sys2(SYS_MG_BG_MOSAIC, size, layer_mask);
}

void mg_bg_main_priority(MgBgLayer layer, bool hi) {
    (void)_vm_sys2(SYS_MG_BG_MAIN_PRIORITY,
                   (uint32_t)layer, hi ? 1u : 0u);
}
