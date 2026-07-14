/* ============================================================
 *  vm_runtime.h — guest-side runtime (internal)
 *
 *  This header is NOT meant to be included by guest application
 *  code. Guests should just #include the standard headers
 *  (<stdio.h>, <stdlib.h>, <string.h>, <time.h>) which live in
 *  the same lib/include/ directory.
 *
 *  This header declares the syscall numbers and a few internal
 *  helpers used by vm_runtime.c. It's a single place to keep
 *  the syscall ABI numbers in sync with the host's vm_ecall.h.
 *
 *  ---------------------------------------------------------------
 *  Linker-script requirements
 *  ---------------------------------------------------------------
 *
 *  vm_runtime.c's _start needs two symbols defined by the linker
 *  script to zero BSS at startup:
 *
 *      __bss_start
 *      __bss_end
 *
 *  examples/common/guest.ld provides these. If you build a guest
 *  with a different script, define them yourself or stub out
 *  the BSS-zero loop in _start.
 *
 *  ---------------------------------------------------------------
 *  Weak _start
 *  ---------------------------------------------------------------
 *
 *  The runtime's _start is declared __attribute__((weak)) so that
 *  a legacy guest that supplies its own _start (and doesn't have
 *  main()) still links cleanly. The linker drops the runtime's
 *  weak _start when a strong one exists; --gc-sections then
 *  removes the unreferenced extern reference to main() that
 *  would otherwise be undefined.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_RUNTIME_H
#define MICROGARBAGE_VM_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* ---------- Syscall numbers (must match host vm_ecall.h) ---------- */

#define SYS_OPENAT                    56
#define SYS_CLOSE                     57
#define SYS_LSEEK                     62
#define SYS_READ                      63
#define SYS_WRITE                     64
#define SYS_FFLUSH                    82
#define SYS_EXIT                      93
#define SYS_READDIR                  120
#define SYS_MKDIRAT                   34
#define SYS_UNLINKAT                  35

#define SYS_SELF                    1024
#define SYS_TICKS_NOW               1043
#define SYS_TICK_HZ                 1044
#define SYS_SLEEP_TICKS             1045
#define SYS_SLEEP_UNTIL             1046

#define SYS_ALLOC                   1056
#define SYS_FREE                    1057

#define SYS_MEMCPY                  1088
#define SYS_MEMSET                  1089
#define SYS_MEMMOVE                 1090
#define SYS_MEMCMP                  1091
#define SYS_STRLEN                  1092
#define SYS_STRCMP                  1093
#define SYS_STRCHR                  1094

#define SYS_SPAWN_AND_WAIT          1104
#define SYS_TTY_SET_RAW             1105

/* Platform services */
#define SYS_FORMAT_AND_WRITE        1108
#define SYS_FORMAT_TO_BUF           1109
#define SYS_REALTIME_NOW            1110
#define SYS_ALLOC_SIZE              1111
#define SYS_RAND                    1112
#define SYS_TIMING_DEADLINE_REMAINING 1113

/* Audio (must match host vm_ecall.h 1160..1167) */
#define SYS_AUDIO_LOAD_SAMPLE       1160
#define SYS_AUDIO_LOAD_MUSIC        1161
#define SYS_AUDIO_FREE              1162
#define SYS_AUDIO_TRIGGER_SFX       1163
#define SYS_AUDIO_PLAY_MUSIC        1164
#define SYS_AUDIO_STOP             1165
#define SYS_AUDIO_SET_GAIN          1166
#define SYS_AUDIO_GET_LEVELS        1167
#define SYS_AUDIO_FFT_ENABLE        1168
#define SYS_AUDIO_LOAD_WAV          1169
#define SYS_AUDIO_STREAM_WAV        1170
#define SYS_AUDIO_PCM_STREAM_OPEN   1171
#define SYS_AUDIO_PCM_STREAM_FEED   1172
#define SYS_AUDIO_PCM_STREAM_CLOSE  1173

/* Cart-coprocessor staging (must match host vm_ecall.h 1180..1184) */
#define SYS_COPRO_STAGE_PAYLOAD     1180
#define SYS_COPRO_STAGE_DMA_SLOT    1181
#define SYS_COPRO_FRAME_COMMIT      1182
#define SYS_COPRO_READ_PADS         1183
#define SYS_COPRO_WAIT_VBLANK       1184

/* L2 allocator (must match host vm_ecall.h 1185..1187) */
#define SYS_L2_ALLOC                1185
#define SYS_L2_FREE                 1186
#define SYS_L2_STATS                1187

/* Cart reset awareness (must match host vm_ecall.h 1188..1189) */
#define SYS_COPRO_RESET_COUNT       1188
#define SYS_COPRO_RESET_ACK         1189

/* Game-facing cart API (1190..1219). Wrapped by the mg_* library at
 * examples/common/guest/mg_*.{h,c}; game code uses the mg_* names, not
 * these. Specs: docs/game-api.md. */

/* OAM (1190..1199). */
#define SYS_MG_SPRITE_SET           1190
#define SYS_MG_SPRITE_MOVE          1191
#define SYS_MG_SPRITE_HIDE          1192
#define SYS_MG_SPRITE_GET           1193
#define SYS_MG_SPRITES_CLEAR_ALL    1194
#define SYS_MG_SPRITE_SIZES         1195
#define SYS_MG_SPRITE_CHR_BASE      1196
#define SYS_MG_SPRITE_ALLOC         1197
#define SYS_MG_SPRITE_FREE          1198
#define SYS_MG_OAM_SNAP_RESTORE     1199

/* BG (1200..1209). */
#define SYS_MG_BG_MODE              1200
#define SYS_MG_BG_SETUP             1201
#define SYS_MG_BG_ENABLE            1202
#define SYS_MG_BG_SET_TILE          1203
#define SYS_MG_BG_GET_TILE          1204
#define SYS_MG_BG_BLIT              1205
#define SYS_MG_BG_UPLOAD            1206
#define SYS_MG_BG_SCROLL            1207
#define SYS_MG_BG_MOSAIC            1208
#define SYS_MG_BG_MAIN_PRIORITY     1209

/* Mode 7 + HDMA (1210..1214). */
#define SYS_MG_MODE7_SET            1210
#define SYS_MG_MODE7_WRAP           1211
#define SYS_MG_HDMA_SETUP           1212
#define SYS_MG_HDMA_UPLOAD          1213
#define SYS_MG_HDMA_ENABLE          1214

/* GFX (1215..1218) + panic (1219). */
#define SYS_MG_CHR_UPLOAD           1215
#define SYS_MG_PALETTE_WRITE        1216
#define SYS_MG_PALETTE_SNAP_RESTORE 1217
#define SYS_MG_PACK_CHR             1218
#define SYS_MG_PANIC                1219
#define SYS_MG_FRAME_STATE          1220
#define SYS_MG_PPU_CLEAN_SLATE      1221
#define SYS_MG_CHR_UPLOAD_TRANSIENT 1222

/* Stream arbiter (1223..1226). See include/io/stream_arbiter.h. */
#define SYS_STREAM_REGISTER  1223
#define SYS_STREAM_CONSUME   1224
#define SYS_STREAM_CLOSE     1225
#define SYS_STREAM_EOF       1226

/* NMI builder (1227). See examples/common/guest/mg_nmi.h. */
#define SYS_MG_NMI_INSTALL   1227

/* HIRQ builder (1228, v2.26 Phase 2.5). See examples/common/guest/mg_nmi.h. */
#define SYS_MG_HIRQ_INSTALL    1228
#define SYS_MG_HIRQ_CONFIGURE  1229  /* (vtime, htime, nmitimen_bits) */

/* Phase 3a unified kernel — layout + siphon (1230..1231). */
#define SYS_MG_KERNEL_LAYOUT     1230  /* (top_lb, bottom_lb) */
#define SYS_MG_SIPHON_CONFIGURE  1231  /* (bytes_per_line, src_off, wram_dst) */

/* Host-driven FMV playback (1232..1234). See mg_fmv.h. */
#define SYS_FMV_PLAY             1232  /* (fd) → 0/-errno; host owns fd */
#define SYS_FMV_STOP             1233  /* () → 0 */
#define SYS_FMV_STATUS           1234  /* () → 0 idle / 1 playing / 2 eof */
#define SYS_FMV_SET_HTIME        1235  /* (htime) → 0; live siphon HTIME tune */

/* Port-2 SNES Mouse read (1236). → packed buttons|dx<<8|dy<<16. */
#define SYS_MG_READ_MOUSE        1236

/* Native 3D renderer service (1240..1246). Keep in sync with
 * include/vm/vm_ecall.h. The renderer runs in firmware; the guest issues
 * scene commands (Q16.16) and asks for a frame. See mg_r3d.h. */
#define SYS_R3D_RESET            1240  /* () → 0 */
#define SYS_R3D_CAMERA           1241  /* (ex,ey,ez,yaw,pitch,roll,focal) → 0 */
#define SYS_R3D_ADD              1242  /* (mesh_id) → handle or -1 */
#define SYS_R3D_MOVE             1243  /* (obj,x,y,z) → 0 */
#define SYS_R3D_ROTATE           1244  /* (obj,rx,ry,rz) → 0 */
#define SYS_R3D_SHOW             1245  /* (obj,visible) → 0 */
#define SYS_R3D_RENDER           1246  /* () → 0 staged / -1 in-flight */

/* Hardware I/O (1250..1279). Transaction-oriented GPIO/I2C/SPI/ADC/PWM.
 * Keep in sync with include/vm/vm_ecall.h. Wrappers: hwio.h. */
#define SYS_GPIO_CONFIG          1250  /* (pin, mode) → 0/-errno */
#define SYS_GPIO_WRITE           1251  /* (pin, level) → 0/-errno */
#define SYS_GPIO_READ            1252  /* (pin) → 0/1 or -errno */
#define SYS_GPIO_TOGGLE          1253  /* (pin) → 0/-errno */
#define SYS_GPIO_WRITE_MASK      1254  /* (bank, mask, values) → 0/-errno */
#define SYS_GPIO_READ_MASK       1255  /* (bank, mask) → sampled bits */
#define SYS_I2C_CONFIG           1258  /* (bus, hz) → 0/-errno */
#define SYS_I2C_XFER             1259  /* (bus, addr, wbuf, wlen, rbuf, rlen) → 0/-errno */
#define SYS_SPI_CONFIG           1262  /* (bus, mode, hz) → 0/-errno */
#define SYS_SPI_XFER             1263  /* (bus, cs, tx, rx, len) → 0/-errno */
#define SYS_ADC_READ             1266  /* (channel) → value or -errno */
#define SYS_PWM_CONFIG           1268  /* (channel, hz) → 0/-errno */
#define SYS_PWM_SET              1269  /* (channel, duty_q16) → 0/-errno */

/* ---------- Inline syscall helpers ----------
 * Six variants by arity. All return a0 unchanged from the syscall.
 * Clobber a0 (return), preserve a1..a6 (they're used as inputs).
 */
static inline uint32_t _vm_sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys1(uint32_t n, uint32_t x0) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys2(uint32_t n, uint32_t x0, uint32_t x1) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys3(uint32_t n, uint32_t x0, uint32_t x1, uint32_t x2) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys4(uint32_t n, uint32_t x0, uint32_t x1,
                                 uint32_t x2, uint32_t x3) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a3 asm("a3") = x3;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys5(uint32_t n, uint32_t x0, uint32_t x1,
                                 uint32_t x2, uint32_t x3, uint32_t x4) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a3 asm("a3") = x3;
    register uint32_t a4 asm("a4") = x4;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                  : "memory");
    return a0;
}
static inline uint32_t _vm_sys6(uint32_t n, uint32_t x0, uint32_t x1,
                                 uint32_t x2, uint32_t x3, uint32_t x4,
                                 uint32_t x5) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a3 asm("a3") = x3;
    register uint32_t a4 asm("a4") = x4;
    register uint32_t a5 asm("a5") = x5;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                  : "memory");
    return a0;
}
static inline uint32_t _vm_sys7(uint32_t n, uint32_t x0, uint32_t x1,
                                 uint32_t x2, uint32_t x3, uint32_t x4,
                                 uint32_t x5, uint32_t x6) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a3 asm("a3") = x3;
    register uint32_t a4 asm("a4") = x4;
    register uint32_t a5 asm("a5") = x5;
    register uint32_t a6 asm("a6") = x6;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                  : "memory");
    return a0;
}

#endif /* MICROGARBAGE_VM_RUNTIME_H */
