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
#define SYS_MG_PPU_CLEAN_SLATE     1221

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

/* MgMode7Params mirror — matches examples/common/guest/mg_mode7.h. */
typedef struct {
    int16_t a, b, c, d;
    int16_t cx, cy;
    int16_t hofs, vofs;
} MgMode7ParamsHost;

static void h_mode7_set(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint32_t pp = cpu->regs[VM_REG_A0];
    MgMode7ParamsHost p;
    if (!guest_read(cpu, pp, &p, sizeof(p))) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    MgState *st = mg_state();
    st->m7a  = p.a;
    st->m7b  = p.b;
    st->m7c  = p.c;
    st->m7d  = p.d;
    st->m7cx = p.cx;
    st->m7cy = p.cy;
    /* Mode 7 scroll routes through the BG1 scroll path — write into
     * layer 0's hofs/vofs so the existing PPU batch picks them up. */
    st->bg[0].hofs = p.hofs;
    st->bg[0].vofs = p.vofs;
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_mode7_wrap(VmCpu *cpu, void *sys_) {
    (void)sys_;
    /* MgMode7Wrap (0..3) maps directly onto the two M7SEL low bits. */
    mg_state()->m7sel = (uint8_t)(cpu->regs[VM_REG_A0] & 0x03);
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

/* MgHdmaCfg mirror — matches examples/common/guest/mg_hdma.h. */
typedef struct {
    uint8_t  channel;
    uint8_t  dest;        /* MgHdmaDest, == $21xx low byte we want */
    uint8_t  xfer;        /* MgHdmaXfer 0..3                       */
    uint8_t  indirect;    /* bool                                  */
} MgHdmaCfgHost;

/* Map MgHdmaXfer (0..3) onto SNES DMAP transfer-mode bits 0..2:
 *   MG_HDMA_XFER_1B_1R = 0  → 000 (1 byte)
 *   MG_HDMA_XFER_2B_1R = 1  → 010 (2 bytes, same reg)
 *   MG_HDMA_XFER_2B_2R = 2  → 001 (2 bytes, 2 regs)
 *   MG_HDMA_XFER_4B_2R = 3  → 011 (4 bytes, 2 regs ×2)
 * Plus the indirect bit (bit 6 of DMAP). Direction stays 0 (CPU->PPU). */
static uint8_t hdma_dmap(uint8_t xfer, bool indirect) {
    static const uint8_t tab[4] = { 0, 2, 1, 3 };
    uint8_t d = tab[xfer & 3];
    if (indirect) d |= 0x40;
    return d;
}

static void h_hdma_setup(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint32_t cfgp = cpu->regs[VM_REG_A0];
    MgHdmaCfgHost cfg;
    if (!guest_read(cpu, cfgp, &cfg, sizeof(cfg))) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    /* Channel 0 reserved for the kernel's DMA-list dispatch; channel
     * 7 reserved for INIDISP letterbox. Games use 1..6. */
    if (cfg.channel == 0 || cfg.channel >= 7) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    mg_state()->hdma[cfg.channel].bbad = cfg.dest;
    mg_state()->hdma[cfg.channel].dmap = hdma_dmap(cfg.xfer, cfg.indirect != 0);
    /* table_off stays whatever upload set it to; enabled stays as-is. */
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_hdma_upload(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t  channel = (uint8_t)cpu->regs[VM_REG_A0];
    uint32_t tablep  = cpu->regs[VM_REG_A1];
    uint16_t len     = (uint16_t)cpu->regs[VM_REG_A2];
    if (channel == 0 || channel >= 7) {
        fprintf(stderr, "mgapi h_hdma_upload: bad channel %u (1..6 valid)\n",
                (unsigned)channel);
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    if (len == 0) { cpu->regs[VM_REG_A0] = MG_R_OK; return; }

    const void *src = vm_translate_read(cpu, tablep, len);
    if (!src) {
        fprintf(stderr, "mgapi h_hdma_upload: vm_translate_read(0x%08x, %u) "
                        "returned NULL\n", tablep, (unsigned)len);
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }

    uint16_t off = mg_state_stage_hdma_table(src, len);
    if (off == UINT16_MAX) {
        fprintf(stderr, "mgapi h_hdma_upload: pool overflow ch=%u len=%u "
                        "(pool cap 1280)\n",
                (unsigned)channel, (unsigned)len);
        cpu->regs[VM_REG_A0] = MG_R_ERR_DMA_BYTES;
        return;
    }
    mg_state()->hdma[channel].table_off = off;
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

static void h_hdma_enable(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint8_t channel = (uint8_t)cpu->regs[VM_REG_A0];
    bool    on      = (cpu->regs[VM_REG_A1] != 0);
    if (channel == 0 || channel >= 7) {
        cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
        return;
    }
    mg_state()->hdma[channel].enabled = on;
    cpu->regs[VM_REG_A0] = MG_R_OK;
}

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
#define MG_FS_GET_SLOTS    0
#define MG_FS_GET_BYTES    1
#define MG_FS_GET_TOP      2
#define MG_FS_GET_BOT      3
#define MG_FS_SET_BLANK    4
#define MG_FS_PANIC_READ   5   /* (op, guest_buf, cap) -> bytes written  */
#define MG_FS_PANIC_CTX    6   /* (op, guest_ctx_ptr) -> 0 if no panic   */

/* -------- Persistent panic state (host-side) --------
 *
 * h_panic saves the message + a crash context into static storage
 * that survives the VM halting. A future error.elf reads it via
 * MG_FS_PANIC_READ / MG_FS_PANIC_CTX. Until error.elf is built and
 * the runtime auto-loads it on panic, the customer still sees the
 * stderr message (kept for dev visibility) and a frozen frame on
 * the SNES. */

#define MG_PANIC_MSG_CAP   240

typedef struct {
    uint32_t vm_id;             /* the panicking VM                       */
    uint32_t pc;                /* panic site PC                          */
    uint32_t a0_a6[7];          /* a0..a6 at panic time                   */
    uint32_t msg_len;            /* bytes in msg[] (no NUL)                */
    uint8_t  reserved[4];
} MgPanicCtx;
_Static_assert(sizeof(MgPanicCtx) == 44,
               "MgPanicCtx layout — keep in sync with the guest reader");

static bool        s_panic_active = false;
static MgPanicCtx  s_panic_ctx;
static char        s_panic_msg[MG_PANIC_MSG_CAP];

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
        case MG_FS_PANIC_READ: {
            /* Args: a1 = guest buffer ptr, a2 = capacity in bytes.
             * Copies the saved message (no trailing NUL) and returns
             * the byte count. Returns 0 if no panic is pending. */
            if (!s_panic_active) {
                cpu->regs[VM_REG_A0] = 0;
                return;
            }
            uint32_t bufp = cpu->regs[VM_REG_A1];
            uint32_t cap  = cpu->regs[VM_REG_A2];
            uint32_t n    = s_panic_ctx.msg_len;
            if (n > cap) n = cap;
            if (n > 0 && !guest_write(cpu, bufp, s_panic_msg, n)) {
                cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
                return;
            }
            cpu->regs[VM_REG_A0] = n;
            return;
        }
        case MG_FS_PANIC_CTX: {
            /* Args: a1 = guest MgPanicCtx*. Returns 0 if no panic, or
             * MG_R_OK on success. Useful for error.elf to show vm_id /
             * PC / register dump alongside the message. */
            if (!s_panic_active) {
                cpu->regs[VM_REG_A0] = 0;
                return;
            }
            uint32_t ctxp = cpu->regs[VM_REG_A1];
            if (!guest_write(cpu, ctxp, &s_panic_ctx, sizeof(s_panic_ctx))) {
                cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
                return;
            }
            cpu->regs[VM_REG_A0] = MG_R_OK;
            return;
        }
        default:
            cpu->regs[VM_REG_A0] = MG_R_ERR_INVALID;
            return;
    }
}

/* Test/host helpers: a future "auto-load error.elf" step will check
 * s_panic_active and consume the buffer. For now exposed so
 * mgapi_host_test or other diagnostics can read the saved state
 * without going through the guest ecall path. */
bool mgapi_panic_active(void) { return s_panic_active; }

const char *mgapi_panic_msg(uint32_t *out_len) {
    if (out_len) *out_len = s_panic_active ? s_panic_ctx.msg_len : 0;
    return s_panic_active ? s_panic_msg : NULL;
}

void mgapi_panic_clear(void) {
    s_panic_active = false;
    /* Leave the buffers in place — clearing is just a flag flip so a
     * read after clear shows "no panic" but the bytes are still there
     * for diagnostics. */
}

/* Find the end of a NUL-terminated guest string starting at guest_va,
 * capped at `max`. Returns the length (excluding NUL). On the host
 * side we have the full mapped guest memory via vm_translate_read,
 * so this is a linear scan. */
static uint32_t guest_strlen(VmCpu *cpu, uint32_t guest_va, uint32_t max) {
    const char *p = (const char *)vm_translate_read(cpu, guest_va, 1);
    if (!p) return 0;
    /* Scan one byte at a time so a translation boundary doesn't trip
     * us up — vm_translate_read with size=1 always returns a valid
     * pointer to that byte. */
    uint32_t n = 0;
    while (n < max) {
        const char *q = (const char *)vm_translate_read(cpu, guest_va + n, 1);
        if (!q || *q == 0) break;
        n++;
    }
    return n;
}

static void h_panic(VmCpu *cpu, void *sys_) {
    (void)sys_;
    uint32_t msgp = cpu->regs[VM_REG_A0];

    /* Capture the message length first, then copy. The message is
     * what the customer wants to see most; the context fields below
     * are for debugging. */
    uint32_t mlen = guest_strlen(cpu, msgp, MG_PANIC_MSG_CAP);
    if (mlen > 0) {
        (void)guest_read(cpu, msgp, s_panic_msg, mlen);
    }
    /* Defensive: if the read failed mid-way, mlen still reflects the
     * intended length; pad anything past mlen with NUL for the
     * stderr print below to terminate cleanly. */
    if (mlen < MG_PANIC_MSG_CAP) {
        s_panic_msg[mlen] = '\0';
    }

    /* Build the crash context. PC is the address of the ecall
     * instruction; a0..a6 are the caller's argument and scratch regs
     * as they entered the syscall. */
    s_panic_ctx.vm_id    = cpu->vm_id;
    s_panic_ctx.pc       = cpu->pc;
    s_panic_ctx.a0_a6[0] = cpu->regs[VM_REG_A0];
    s_panic_ctx.a0_a6[1] = cpu->regs[VM_REG_A1];
    s_panic_ctx.a0_a6[2] = cpu->regs[VM_REG_A2];
    s_panic_ctx.a0_a6[3] = cpu->regs[VM_REG_A3];
    s_panic_ctx.a0_a6[4] = cpu->regs[VM_REG_A4];
    s_panic_ctx.a0_a6[5] = cpu->regs[VM_REG_A5];
    s_panic_ctx.a0_a6[6] = cpu->regs[VM_REG_A6];
    s_panic_ctx.msg_len  = mlen;

    s_panic_active = true;

    /* Dev-visible note: stderr stays as a debugging aid until
     * error.elf is built and the runtime loads it on panic. */
    fprintf(stderr, "mg_panic [vm %u pc=%08x]: %s\n",
            (unsigned)cpu->vm_id, (unsigned)cpu->pc, s_panic_msg);
    fflush(stderr);

    cpu->halted = true;
}

/* Voluntary "clean slate" PPU reset. A child VM calls this when it
 * doesn't want to inherit the parent's PPU state (VRAM tiles, CGRAM
 * palette, OAM sprites, BG/M7/HDMA config). The host-side unload hook
 * (vm_init.c) already resets mg_state's shadow when a VM exits, but the
 * actual PPU memory (VRAM/CGRAM/OAM) persists across VM boundaries
 * because the host can't touch those without staging a DMA. This call
 * stages those DMAs:
 *
 *   1. Reset all shadow state via mg_state_reset (cgram zero, oam y=240
 *      hidden, bg disabled, m7 identity, hdma disabled, etc.)
 *   2. Mark cgram/oam shadow ranges full-dirty so the next commit DMA's
 *      the zeroed/hidden values into PPU CGRAM/OAM.
 *   3. Stage a VRAM-clear DMA slot (fixed-source $0000 + 65536-byte
 *      transfer → fills all 32K VRAM words with $0000).
 *
 * Result: after the NEXT mg_frame_commit, PPU VRAM/CGRAM/OAM are all
 * zero, BG layers disabled, force-blank off. The caller can then start
 * uploading its own tiles/palette/sprites from a known-clean state.
 *
 * Cost: 1 DMA slot + 2 bytes of payload (the zero source). The actual
 * 64KB VRAM DMA happens entirely in vblank — 32K word writes at 8
 * master cycles each = 1024us, which fits in the ~2.4ms NTSC vblank
 * with margin. OAM (544 bytes) and CGRAM (512 bytes) DMAs also fit. */
static void h_ppu_clean_slate(VmCpu *cpu, void *sys_) {
    (void)sys_;
    MgState *st = mg_state();

    /* Reset shadows + config. mg_state_reset publishes oam_dirty=full
     * so OAM gets re-DMA'd; we mirror that for CGRAM since the reset
     * leaves CGRAM clean (no dirty marks). */
    mg_state_reset();
    st->cgram_dirty_lo = 0;
    st->cgram_dirty_hi = sizeof(st->cgram_shadow);

    /* Set the pending-VRAM-clear flag; build_frame stages the actual
     * DMA slot during the next commit's emit phase, when slot indices
     * are coordinated with CGRAM/OAM/etc. (Staging directly here would
     * race with build_frame's s_slot_used reset and get clobbered.)
     *
     * KNOWN BUG: the VRAM-clear path is not yet honored by build_frame
     * (see task #6 in the session notes). Until it is, the partial
     * mitigation below covers the most common visible leak — stale
     * tilemap entries from a previous demo. */
    st->pending_vram_clear = true;

    /* Dirty all 4 BG shadow tilemaps so emit_bg_tilemap stages a
     * 2KB-of-zeros DMA into whatever tilemap_word the demo's
     * mg_bg_setup ends up at. emit_bg_tilemap's enabled-layer check
     * (added alongside this) ensures only ACTIVE layers actually emit,
     * so the typical 1-2 enabled layer demos hit a 2-4KB DMA that
     * fits in vblank. This clears the leak where a prior demo's
     * tilemap data (e.g., mode7.elf's CHR/tilemap-interleaved write at
     * VRAM $0000) shows through a new demo's BG that reads from the
     * same VRAM range. Tile-0 CHR persistence is NOT addressed by
     * this — demos that don't upload their own CHR can still see
     * leftover pixel data, but the visible-block-at-top class of
     * symptom is the most common and is fixed by tilemap clear. */
    for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
        st->bg[i].dirty_lo = 0;
        st->bg[i].dirty_hi = (uint16_t)sizeof(st->bg[i].shadow);
    }

    cpu->regs[VM_REG_A0] = MG_R_OK;
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
    { SYS_MG_PPU_CLEAN_SLATE,      h_ppu_clean_slate    },
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
