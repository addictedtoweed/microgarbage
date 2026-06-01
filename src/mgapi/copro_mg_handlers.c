/* ============================================================
 *  copro_mg_handlers.c — SYS_MG_* ecall handlers (1190..1219).
 *
 *  Each handler:
 *    1. Reads arguments out of cpu->regs[VM_REG_A0..A5].
 *    2. Translates any guest pointers via vm_translate_read /
 *       vm_translate_write.
 *    3. Mutates the shadow PPU state in copro_mg_state, marking
 *       dirty ranges where applicable.
 *    4. Writes the result to cpu->regs[VM_REG_A0].
 *
 *  Result codes match the guest-side MgResult enum:
 *    0  = MG_OK
 *   -1  = MG_ERR_DMA_SLOTS    (8-slot list full)
 *   -2  = MG_ERR_DMA_BYTES    (byte budget exceeded)
 *   -3  = MG_ERR_INVALID      (bad arg)
 *
 *  Handlers that don't perform DMA always return 0.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "copro_mg_handlers.h"
#include "copro_mg_state.h"
#include "cart_window.h"

#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/vm_system.h"

#include <stdio.h>
#include <string.h>

/* SYS_MG_* numbers must match vm_ecall.h; redeclared here so this
 * file compiles independently of the layout there. */
#define SYS_MG_SPRITE_SET          1190
#define SYS_MG_SPRITE_MOVE         1191
#define SYS_MG_SPRITE_HIDE         1192
#define SYS_MG_SPRITE_GET          1193
#define SYS_MG_SPRITES_CLEAR_ALL   1194
#define SYS_MG_SPRITE_SIZES        1195
#define SYS_MG_SPRITE_CHR_BASE     1196
#define SYS_MG_SPRITE_ALLOC        1197
#define SYS_MG_SPRITE_FREE         1198
#define SYS_MG_OAM_SNAP_RESTORE    1199
#define SYS_MG_BG_MODE             1200
#define SYS_MG_BG_SETUP            1201
#define SYS_MG_BG_ENABLE           1202
#define SYS_MG_BG_SET_TILE         1203
#define SYS_MG_BG_GET_TILE         1204
#define SYS_MG_BG_BLIT             1205
#define SYS_MG_BG_UPLOAD           1206
#define SYS_MG_BG_SCROLL           1207
#define SYS_MG_BG_MOSAIC           1208
#define SYS_MG_BG_MAIN_PRIORITY    1209
#define SYS_MG_MODE7_SET           1210
#define SYS_MG_MODE7_WRAP          1211
#define SYS_MG_HDMA_SETUP          1212
#define SYS_MG_HDMA_UPLOAD         1213
#define SYS_MG_HDMA_ENABLE         1214
#define SYS_MG_CHR_UPLOAD          1215
#define SYS_MG_PALETTE_WRITE       1216
#define SYS_MG_PALETTE_SNAP_RESTORE 1217
#define SYS_MG_PACK_CHR            1218
#define SYS_MG_PANIC               1219
#define SYS_MG_FRAME_STATE         1220

/* MgResult values mirrored from mg_panic.h. */
#define MG_R_OK             0
#define MG_R_ERR_DMA_SLOTS (uint32_t)(-1)
#define MG_R_ERR_DMA_BYTES (uint32_t)(-2)
#define MG_R_ERR_INVALID   (uint32_t)(-3)

/* Helpers --------------------------------------------------------- */

/* Copy `n` bytes from a guest VA into `dst`. vm_translate_read
 * returns a host pointer to the translated guest memory (or NULL
 * if the range is unreadable); we memcpy from it. */
static bool guest_read(VmCpu *cpu, uint32_t guest_va, void *dst, size_t n) {
    const void *src = vm_translate_read(cpu, guest_va, (uint32_t)n);
    if (!src) return false;
    memcpy(dst, src, n);
    return true;
}

/* Same shape for the write path. */
static bool guest_write(VmCpu *cpu, uint32_t guest_va, const void *src, size_t n) {
    void *dst = vm_translate_write(cpu, guest_va, (uint32_t)n);
    if (!dst) return false;
    memcpy(dst, src, n);
    return true;
}

/* Sprite ---------------------------------------------------------- */

/* Write an MgSpriteHost into the shadow OAM at the given slot,
 * marking the slot's 4 main bytes + 2-bit high OAM cell as dirty.
 *
 * Main OAM byte layout (4 bytes per sprite):
 *   byte 0:  X low 8 bits
 *   byte 1:  Y
 *   byte 2:  tile number low 8 bits
 *   byte 3:  vhppccct (v=vflip, h=hflip, ppp=palette, cc=priority,
 *                     t=tile high bit)
 *
 * High OAM byte (2 bits per sprite, 128 sprites in 32 bytes):
 *   bit 0:  X high bit (sign extension)
 *   bit 1:  size flag (small vs large) */
static void shadow_write_sprite(uint8_t slot, const MgSpriteHost *s) {
    uint8_t *oam = mg_state()->oam_shadow;

    /* Main OAM slot at offset slot * 4. */
    uint16_t off = (uint16_t)slot * 4u;
    oam[off + 0] = (uint8_t)(s->x & 0xFF);
    oam[off + 1] = s->y;
    oam[off + 2] = (uint8_t)(s->tile & 0xFF);

    uint8_t attrs = (uint8_t)(
          ((s->tile >> 8) & 0x01)            /* tile high bit */
        | ((uint8_t)s->palette  << 1)
        | ((uint8_t)s->priority << 4)
        | ((uint8_t)s->hflip    << 6)
        | ((uint8_t)s->vflip    << 7)
    );
    oam[off + 3] = attrs;

    /* High OAM: 2 bits per sprite. slot/4 picks the byte, (slot%4)*2
     * picks the bit position. */
    uint16_t hi_byte_off = 512u + (uint16_t)(slot / 4);
    unsigned shift       = (slot % 4) * 2;
    uint8_t  hi          = oam[hi_byte_off];
    uint8_t  bits        = (uint8_t)(
          (((s->x < 0) || (s->x > 255)) ? 1u : 0u)
        | ((s->size_large ? 1u : 0u) << 1)
    );
    hi = (uint8_t)((hi & ~(0x3u << shift)) | (bits << shift));
    oam[hi_byte_off] = hi;

    mg_state_dirty_oam(off, off + 4);
    mg_state_dirty_oam(hi_byte_off, hi_byte_off + 1);
}

static void h_sprite_set(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  slot   = (uint8_t)cpu->regs[VM_REG_A0];
    uint32_t sptr   = cpu->regs[VM_REG_A1];
    MgSpriteHost s;
    if (!guest_read(cpu, sptr, &s, sizeof(s))) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    shadow_write_sprite(slot, &s);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprite_move(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  slot = (uint8_t)cpu->regs[VM_REG_A0];
    int16_t  x    = (int16_t)(uint16_t)cpu->regs[VM_REG_A1];
    uint8_t  y    = (uint8_t)cpu->regs[VM_REG_A2];

    uint8_t *oam = mg_state()->oam_shadow;
    uint16_t off = (uint16_t)slot * 4u;
    oam[off + 0] = (uint8_t)(x & 0xFF);
    oam[off + 1] = y;
    /* Update high-OAM X-sign bit; size bit untouched. */
    uint16_t hi_byte_off = 512u + (uint16_t)(slot / 4);
    unsigned shift       = (slot % 4) * 2;
    uint8_t  x_high      = (((x < 0) || (x > 255)) ? 1u : 0u);
    uint8_t  cur         = oam[hi_byte_off];
    cur = (uint8_t)((cur & ~(1u << shift)) | (x_high << shift));
    oam[hi_byte_off] = cur;

    mg_state_dirty_oam(off, off + 2);
    mg_state_dirty_oam(hi_byte_off, hi_byte_off + 1);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprite_hide(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  slot = (uint8_t)cpu->regs[VM_REG_A0];
    uint8_t *oam = mg_state()->oam_shadow;
    uint16_t off = (uint16_t)slot * 4u;
    oam[off + 1] = 240;          /* y = offscreen */
    mg_state_dirty_oam(off + 1, off + 2);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprite_get(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  slot = (uint8_t)cpu->regs[VM_REG_A0];
    uint32_t outp = cpu->regs[VM_REG_A1];
    const uint8_t *oam = mg_state()->oam_shadow;
    uint16_t off = (uint16_t)slot * 4u;

    MgSpriteHost s = {0};
    s.x          = (int16_t)(int8_t)oam[off + 0];   /* sign-extend low byte */
    s.y          = oam[off + 1];
    uint8_t a3   = oam[off + 3];
    s.tile       = (uint16_t)oam[off + 2] | ((uint16_t)(a3 & 1) << 8);
    s.palette    = (uint8_t)((a3 >> 1) & 0x07);
    s.priority   = (uint8_t)((a3 >> 4) & 0x03);
    s.hflip      = (uint8_t)((a3 >> 6) & 1);
    s.vflip      = (uint8_t)((a3 >> 7) & 1);

    uint16_t hi_byte_off = 512u + (uint16_t)(slot / 4);
    unsigned shift       = (slot % 4) * 2;
    uint8_t  hi          = (uint8_t)((oam[hi_byte_off] >> shift) & 0x03);
    s.size_large = (uint8_t)((hi >> 1) & 1);
    /* Reconstruct the 9-bit signed X using the high bit. */
    if (hi & 1) s.x = (int16_t)(s.x - 256);

    if (!guest_write(cpu, outp, &s, sizeof(s))) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprites_clear_all(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t *oam = mg_state()->oam_shadow;
    for (unsigned i = 0; i < 128; i++) {
        oam[i * 4 + 1] = 240;
    }
    mg_state_dirty_oam(0, MG_OAM_BYTES);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprite_sizes(VmCpu *cpu, void *sys_) {
    (void)sys_;
    mg_state()->spr.sizes_code = (uint8_t)cpu->regs[VM_REG_A0];
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprite_chr_base(VmCpu *cpu, void *sys_) {
    (void)sys_;
    mg_state()->spr.chr_base0_word = (uint16_t)cpu->regs[VM_REG_A0];
    mg_state()->spr.chr_base1_word = (uint16_t)cpu->regs[VM_REG_A1];
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_sprite_alloc(VmCpu *cpu, void *sys_) {
    (void)sys_;
    /* TODO: real allocator. For now: always succeed at slot 0; not
     * useful for actually cooperating but lets game code compile and
     * the customer iterate single-VM. */
    (void)cpu->regs[VM_REG_A0];
    cpu->regs[VM_REG_A0] = 0;
}

static void h_sprite_free(VmCpu *cpu, void *sys_) {
    (void)sys_;
    (void)cpu;
    /* No-op while alloc is stubbed. */
}

static void h_oam_snap_restore(VmCpu *cpu, void *sys_) {
    (void)sys_;
    /* TODO. */
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

/* BG -------------------------------------------------------------- */

static void h_bg_mode(VmCpu *cpu, void *sys_) {
    (void)sys_;
    mg_state()->bgmode = (uint8_t)(cpu->regs[VM_REG_A0] & 7);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_setup(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  layer = (uint8_t)cpu->regs[VM_REG_A0];
    if (layer >= MG_BG_LAYERS) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    MgBgLayerState *bg = &mg_state()->bg[layer];
    bg->tilemap_word = (uint16_t)cpu->regs[VM_REG_A1];
    bg->size_code    = (uint8_t)cpu->regs[VM_REG_A2];
    bg->chr_word     = (uint16_t)cpu->regs[VM_REG_A3];
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_enable(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t layer = (uint8_t)cpu->regs[VM_REG_A0];
    uint8_t mask  = (uint8_t)cpu->regs[VM_REG_A1];
    if (layer >= MG_BG_LAYERS) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    mg_state()->bg[layer].enabled_main = (mask & 1) != 0;
    mg_state()->bg[layer].enabled_sub  = (mask & 2) != 0;
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_set_tile(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  layer = (uint8_t)cpu->regs[VM_REG_A0];
    uint8_t  x     = (uint8_t)cpu->regs[VM_REG_A1];
    uint8_t  y     = (uint8_t)cpu->regs[VM_REG_A2];
    uint16_t word  = (uint16_t)cpu->regs[VM_REG_A3];

    if (layer >= MG_BG_LAYERS || x >= 32 || y >= 32) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    uint16_t off = (uint16_t)((y * 32u + x) * 2u);
    uint8_t *map = mg_state()->bg[layer].shadow;
    map[off + 0] = (uint8_t)(word & 0xFF);
    map[off + 1] = (uint8_t)(word >> 8);
    mg_state_dirty_bg(layer, off, off + 2);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_get_tile(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  layer = (uint8_t)cpu->regs[VM_REG_A0];
    uint8_t  x     = (uint8_t)cpu->regs[VM_REG_A1];
    uint8_t  y     = (uint8_t)cpu->regs[VM_REG_A2];
    uint32_t outp  = cpu->regs[VM_REG_A3];
    if (layer >= MG_BG_LAYERS || x >= 32 || y >= 32) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    uint16_t off = (uint16_t)((y * 32u + x) * 2u);
    const uint8_t *map = mg_state()->bg[layer].shadow;
    uint16_t word = (uint16_t)map[off] | ((uint16_t)map[off + 1] << 8);
    if (!guest_write(cpu, outp, &word, sizeof(word))) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_blit(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  layer = (uint8_t)cpu->regs[VM_REG_A0];
    uint32_t xy    = cpu->regs[VM_REG_A1];
    uint32_t cells = cpu->regs[VM_REG_A2];
    uint16_t n     = (uint16_t)cpu->regs[VM_REG_A3];
    if (layer >= MG_BG_LAYERS) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    uint8_t x = (uint8_t)(xy & 0xFF);
    uint8_t y = (uint8_t)((xy >> 8) & 0xFF);
    uint8_t *map = mg_state()->bg[layer].shadow;
    uint16_t pos = (uint16_t)((y * 32u + x) * 2u);

    for (uint16_t i = 0; i < n; i++) {
        uint16_t word;
        if (!guest_read(cpu, cells + i * 2u, &word, 2)) {
            cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
            return;
        }
        if (pos >= MG_BG_TILEMAP_BYTES) pos = 0;  /* row-major wrap */
        map[pos + 0] = (uint8_t)(word & 0xFF);
        map[pos + 1] = (uint8_t)(word >> 8);
        pos = (uint16_t)(pos + 2);
    }
    mg_state_dirty_bg(layer, 0, MG_BG_TILEMAP_BYTES);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_upload(VmCpu *cpu, void *sys_) {
    (void)sys_;
    /* TODO: direct path. For now: route through the shadow path so
     * the game sees the right visual effect, even if it costs more
     * than the design budget. Refinement when the budget matters. */
    h_bg_blit(cpu, sys_);
}

static void h_bg_scroll(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t layer = (uint8_t)cpu->regs[VM_REG_A0];
    int16_t hx    = (int16_t)(uint16_t)cpu->regs[VM_REG_A1];
    int16_t vy    = (int16_t)(uint16_t)cpu->regs[VM_REG_A2];
    if (layer >= MG_BG_LAYERS) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    mg_state()->bg[layer].hofs = hx;
    mg_state()->bg[layer].vofs = vy;
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_mosaic(VmCpu *cpu, void *sys_) {
    (void)cpu; (void)sys_;
    /* TODO: stage MOSAIC register write. */
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_bg_main_priority(VmCpu *cpu, void *sys_) {
    (void)cpu; (void)sys_;
    /* TODO: stage BGMODE high-bit. */
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

/* Mode 7 + HDMA --------------------------------------------------- */

static void h_mode7_set        (VmCpu *cpu, void *s) { (void)s; cpu->regs[VM_REG_A0] = MG_R_OK; }
static void h_mode7_wrap       (VmCpu *cpu, void *s) { (void)s; cpu->regs[VM_REG_A0] = MG_R_OK; }
static void h_hdma_setup       (VmCpu *cpu, void *s) { (void)s; cpu->regs[VM_REG_A0] = MG_R_OK; }
static void h_hdma_upload      (VmCpu *cpu, void *s) { (void)s; cpu->regs[VM_REG_A0] = MG_R_OK; }
static void h_hdma_enable      (VmCpu *cpu, void *s) { (void)s; cpu->regs[VM_REG_A0] = MG_R_OK; }

/* GFX + panic ----------------------------------------------------- */

static void h_chr_upload(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint16_t vram_word = (uint16_t)cpu->regs[VM_REG_A0];
    uint32_t srcp      = cpu->regs[VM_REG_A1];
    uint16_t bytes     = (uint16_t)cpu->regs[VM_REG_A2];

    if (bytes == 0) { cpu->regs[VM_REG_A0] = MG_R_OK; return; }

    /* Pull the bytes out of guest memory; vm_translate_read hands us a
     * host pointer we can pass directly to the staging path. */
    const void *src = vm_translate_read(cpu, srcp, bytes);
    if (!src) { cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID; return; }

    /* Queue a VRAM-write DMA: bbus = $18 (VMDATAL), dmap = $01
     * (2-byte / 2-reg, the SNES VRAM write pattern), prep = the
     * destination VRAM word address. The kernel writes VMAIN = $80
     * (increment after high write) when it sees this bbus. */
    int rc = mg_state_queue_dma(src, bytes, 0x18, 0x01, vram_word);
    if (rc == -1) cpu->regs[VM_REG_A0] = MG_R_ERR_DMA_SLOTS;
    else if (rc == -2) cpu->regs[VM_REG_A0] = MG_R_ERR_DMA_BYTES;
    else cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_palette_write(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  start = (uint8_t)cpu->regs[VM_REG_A0];
    uint16_t count = (uint16_t)cpu->regs[VM_REG_A1];
    uint32_t srcp  = cpu->regs[VM_REG_A2];
    uint8_t  fmt   = (uint8_t)cpu->regs[VM_REG_A3];   /* 0=bgr555, 1=rgb24 */

    if (count == 0) { cpu->regs[VM_REG_A0] = MG_R_OK; return; }
    /* Clamp so we don't run off the end of CGRAM. */
    if ((uint32_t)start + count > 256u) {
        count = (uint16_t)(256u - start);
    }
    uint16_t *cgram = mg_state()->cgram_shadow;

    if (fmt == 0) {
        /* BGR555 words. */
        for (uint16_t i = 0; i < count; i++) {
            uint16_t w;
            if (!guest_read(cpu, srcp + i * 2u, &w, 2)) {
                cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
                return;
            }
            cgram[start + i] = w;
        }
    } else {
        /* RGB24: pack on the way in. */
        for (uint16_t i = 0; i < count; i++) {
            uint8_t rgb[3];
            if (!guest_read(cpu, srcp + i * 3u, rgb, 3)) {
                cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
                return;
            }
            uint16_t w = (uint16_t)(((rgb[2] >> 3) << 10) |
                                    ((rgb[1] >> 3) <<  5) |
                                    ((rgb[0] >> 3)));
            cgram[start + i] = w;
        }
    }

    /* Dirty range in byte coords. */
    uint16_t lo = (uint16_t)(start * 2u);
    uint16_t hi = (uint16_t)((start + count) * 2u);
    mg_state_dirty_cgram(lo, hi);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_palette_snap_restore(VmCpu *cpu, void *s) {
    (void)s;
    /* TODO. */
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_pack_chr(VmCpu *cpu, void *s) {
    (void)s;
    /* TODO: real M7-native packer. */
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

/* Frame-state multiplexed ecall. Op-codes must match the constants
 * in examples/common/guest/mg_frame.c. */
#define MG_FS_GET_SLOTS  0
#define MG_FS_GET_BYTES  1
#define MG_FS_GET_TOP    2
#define MG_FS_GET_BOT    3
#define MG_FS_SET_BLANK  4

static void h_frame_state(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint32_t op = cpu->regs[VM_REG_A0];
    switch (op) {
        case MG_FS_GET_SLOTS:
            cpu->regs[VM_REG_A0] = mg_state_slots_remaining();
            return;
        case MG_FS_GET_BYTES:
            cpu->regs[VM_REG_A0] = mg_state_bytes_remaining();
            return;
        case MG_FS_GET_TOP:
            cpu->regs[VM_REG_A0] = mg_state()->force_blank_top;
            return;
        case MG_FS_GET_BOT:
            cpu->regs[VM_REG_A0] = mg_state()->force_blank_bottom;
            return;
        case MG_FS_SET_BLANK: {
            uint8_t top = (uint8_t)cpu->regs[VM_REG_A1];
            uint8_t bot = (uint8_t)cpu->regs[VM_REG_A2];
            /* Cap to keep at least 16 visible lines — protects games
             * from accidentally requesting a fully-blanked screen. */
            if (top + bot > 208) {
                cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
                return;
            }
            mg_state()->force_blank_top    = top;
            mg_state()->force_blank_bottom = bot;
            cpu->regs[VM_REG_A0] = MG_R_OK;
            return;
        }
        default:
            cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
            return;
    }
}

static void h_panic(VmCpu *cpu, void *sys_) {
    (void)sys_;
    /* TODO: real panic flow — save context to persistent slot,
     * trigger SNES reset, reboot into error.elf. For first iteration:
     * print the message to host stderr and halt the calling VM. */
    uint32_t msgp = cpu->regs[VM_REG_A0];
    char buf[256] = {0};
    if (guest_read(cpu, msgp, buf, sizeof(buf) - 1)) {
        fprintf(stderr, "mg_panic: %s\n", buf);
        fflush(stderr);
    }
    cpu->halted = true;
}

/* Install / uninstall --------------------------------------------- */

/* Compact table to keep install + unwind small. */
struct mg_handler_entry {
    uint16_t       num;
    VmEcallHandler fn;
};

static const struct mg_handler_entry s_handlers[] = {
    { SYS_MG_SPRITE_SET,           h_sprite_set         },
    { SYS_MG_SPRITE_MOVE,          h_sprite_move        },
    { SYS_MG_SPRITE_HIDE,          h_sprite_hide        },
    { SYS_MG_SPRITE_GET,           h_sprite_get         },
    { SYS_MG_SPRITES_CLEAR_ALL,    h_sprites_clear_all  },
    { SYS_MG_SPRITE_SIZES,         h_sprite_sizes       },
    { SYS_MG_SPRITE_CHR_BASE,      h_sprite_chr_base    },
    { SYS_MG_SPRITE_ALLOC,         h_sprite_alloc       },
    { SYS_MG_SPRITE_FREE,          h_sprite_free        },
    { SYS_MG_OAM_SNAP_RESTORE,     h_oam_snap_restore   },
    { SYS_MG_BG_MODE,              h_bg_mode            },
    { SYS_MG_BG_SETUP,             h_bg_setup           },
    { SYS_MG_BG_ENABLE,            h_bg_enable          },
    { SYS_MG_BG_SET_TILE,          h_bg_set_tile        },
    { SYS_MG_BG_GET_TILE,          h_bg_get_tile        },
    { SYS_MG_BG_BLIT,              h_bg_blit            },
    { SYS_MG_BG_UPLOAD,            h_bg_upload          },
    { SYS_MG_BG_SCROLL,            h_bg_scroll          },
    { SYS_MG_BG_MOSAIC,            h_bg_mosaic          },
    { SYS_MG_BG_MAIN_PRIORITY,     h_bg_main_priority   },
    { SYS_MG_MODE7_SET,            h_mode7_set          },
    { SYS_MG_MODE7_WRAP,           h_mode7_wrap         },
    { SYS_MG_HDMA_SETUP,           h_hdma_setup         },
    { SYS_MG_HDMA_UPLOAD,          h_hdma_upload        },
    { SYS_MG_HDMA_ENABLE,          h_hdma_enable        },
    { SYS_MG_CHR_UPLOAD,           h_chr_upload         },
    { SYS_MG_PALETTE_WRITE,        h_palette_write      },
    { SYS_MG_PALETTE_SNAP_RESTORE, h_palette_snap_restore },
    { SYS_MG_PACK_CHR,             h_pack_chr           },
    { SYS_MG_PANIC,                h_panic              },
    { SYS_MG_FRAME_STATE,          h_frame_state        },
};

#define MG_HANDLER_COUNT ((unsigned)(sizeof(s_handlers) / sizeof(s_handlers[0])))

bool mg_handlers_install(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;
    for (unsigned i = 0; i < MG_HANDLER_COUNT; i++) {
        if (!vm_ecall_register(sys->ecall_router,
                               s_handlers[i].num, s_handlers[i].fn)) {
            /* Unwind everything we already installed. */
            for (unsigned j = 0; j < i; j++) {
                vm_ecall_unregister(sys->ecall_router, s_handlers[j].num);
            }
            return false;
        }
    }
    return true;
}

void mg_handlers_uninstall(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return;
    for (unsigned i = 0; i < MG_HANDLER_COUNT; i++) {
        vm_ecall_unregister(sys->ecall_router, s_handlers[i].num);
    }
}
