/* ============================================================
 *  mg_sprite.c — guest-side OAM ecall stubs.
 *  See mg_sprite.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_sprite.h"
#include "vm_runtime.h"

void mg_sprite_set(uint8_t slot, const MgSprite *s) {
    (void)_vm_sys2(SYS_MG_SPRITE_SET, slot, (uint32_t)s);
}

void mg_sprite_get(uint8_t slot, MgSprite *out) {
    (void)_vm_sys2(SYS_MG_SPRITE_GET, slot, (uint32_t)out);
}

void mg_sprite_move(uint8_t slot, int16_t x, uint8_t y) {
    /* Pack x (signed 16) and y (unsigned 8) into a3/a2 in the most
     * obvious way — the host extracts them from the syscall regs. */
    (void)_vm_sys3(SYS_MG_SPRITE_MOVE, slot,
                   (uint32_t)(uint16_t)x, y);
}

void mg_sprite_hide(uint8_t slot) {
    (void)_vm_sys1(SYS_MG_SPRITE_HIDE, slot);
}

void mg_sprites_clear_all(void) {
    (void)_vm_sys0(SYS_MG_SPRITES_CLEAR_ALL);
}

void mg_sprite_sizes(MgSpriteSizes pair) {
    (void)_vm_sys1(SYS_MG_SPRITE_SIZES, (uint32_t)pair);
}

void mg_sprite_chr_base(uint16_t base0_word, uint16_t base1_word) {
    (void)_vm_sys2(SYS_MG_SPRITE_CHR_BASE, base0_word, base1_word);
}

int mg_sprite_alloc(uint8_t count) {
    return (int32_t)_vm_sys1(SYS_MG_SPRITE_ALLOC, count);
}

void mg_sprite_free(uint8_t first, uint8_t count) {
    (void)_vm_sys2(SYS_MG_SPRITE_FREE, first, count);
}
