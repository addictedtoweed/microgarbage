/* ============================================================
 *  vm_host_audio.c — SYS_AUDIO_* handlers (requester side)
 *
 *  Each handler turns a guest audio ecall into a REQ_AUDIO_* channel
 *  round-trip to the audio service. See vm_host_audio.h.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_host_audio.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/channel_msg.h"
#include "audio/audio_sink.h"   /* WAV parser (wav_parse/wav_to_mono_pcm16) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module state (single audio service per host). */
static ServiceChannel *g_channel;
static uint8_t        *g_staging;
static size_t          g_staging_cap;
static uint32_t        g_timeout_ms;
static const char     *g_host_fs_root;

/* Post a request and wait for its response; returns the response's
 * status (a3) and handle (a4) via out params. Returns false on
 * channel failure / timeout. */
static bool audio_call(uint16_t type, uint32_t a0, uint32_t a1,
                       uint32_t a2, uint32_t a3,
                       uint32_t *out_status, uint32_t *out_handle) {
    ChannelMsg m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.a0 = a0; m.a1 = a1; m.a2 = a2; m.a3 = a3;
    if (!channel_request_call(g_channel, &m, g_timeout_ms, NULL, NULL))
        return false;
    if (out_status) *out_status = m.a3;
    if (out_handle) *out_handle = m.a4;
    return true;
}

/* ---- SYS_AUDIO_LOAD_SAMPLE (buf, size) -> object handle or 0 ---- */
static void handle_load_sample(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t guest_buf = cpu->regs[VM_REG_A0];
    uint32_t size      = cpu->regs[VM_REG_A1];

    if (!g_staging || size == 0 || size > g_staging_cap) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    const void *src = vm_translate_read(cpu, guest_buf, size);
    if (!src) { cpu->regs[VM_REG_A0] = 0; return; }

    /* Stage the PCM, then ask the service to copy staging->pool. */
    memcpy(g_staging, src, size);
    uint32_t status = 0, handle = 0;
    if (!audio_call(REQ_AUDIO_LOAD_STAGED, size, cpu->vm_id, 0, 0,
                    &status, &handle)) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? handle : 0;
}

/* ---- SYS_AUDIO_FREE (object) -> 0 ---- */
static void handle_free(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t obj = cpu->regs[VM_REG_A0];
    uint32_t status = 0, h = 0;
    audio_call(REQ_AUDIO_POOL_FREE, obj, 0, 0, 0, &status, &h);
    cpu->regs[VM_REG_A0] = 0;
}

/* ---- SYS_AUDIO_TRIGGER_SFX (object, gain, pan) -> voice or 0 ---- */
static void handle_trigger_sfx(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t obj  = cpu->regs[VM_REG_A0];
    uint32_t gain = cpu->regs[VM_REG_A1];
    uint32_t pan  = cpu->regs[VM_REG_A2];
    uint32_t status = 0, voice = 0;
    if (!audio_call(REQ_AUDIO_TRIGGER_SFX, obj, gain, pan, cpu->vm_id,
                    &status, &voice)) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? voice : 0;   /* 0 = REJECTED/err */
}

/* ---- SYS_AUDIO_PLAY_MUSIC (object, flags) -> voice or 0 ---- */
static void handle_play_music(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t obj   = cpu->regs[VM_REG_A0];
    uint32_t flags = cpu->regs[VM_REG_A1];
    uint32_t status = 0, voice = 0;
    if (!audio_call(REQ_AUDIO_PLAY_MUSIC, obj, flags, 0, cpu->vm_id,
                    &status, &voice)) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? voice : 0;
}

/* ---- SYS_AUDIO_STOP (voice) -> 0 or -errno ---- */
static void handle_stop(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t voice = cpu->regs[VM_REG_A0];
    uint32_t status = 0, h = 0;
    if (!audio_call(REQ_AUDIO_VOICE_STOP, voice, 0, 0, 0, &status, &h)) {
        cpu->regs[VM_REG_A0] = (uint32_t)(-VM_EIO); return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? 0u : (uint32_t)(-VM_EINVAL);
}

/* ---- SYS_AUDIO_SET_GAIN (voice, gain) -> 0 or -errno ---- */
static void handle_set_gain(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t voice = cpu->regs[VM_REG_A0];
    uint32_t gain  = cpu->regs[VM_REG_A1];
    uint32_t status = 0, h = 0;
    if (!audio_call(REQ_AUDIO_SET_GAIN, voice, gain, 0, 0, &status, &h)) {
        cpu->regs[VM_REG_A0] = (uint32_t)(-VM_EIO); return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? 0u : (uint32_t)(-VM_EINVAL);
}

/* ---- SYS_AUDIO_LOAD_MUSIC (intro_obj, loop_obj) -> music handle ----
 * The guest loads two samples via SYS_AUDIO_LOAD_SAMPLE, then pairs
 * them here into a single music object. (loop_obj may be 0 for an
 * intro-only object.) */
static void handle_load_music(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t intro = cpu->regs[VM_REG_A0];
    uint32_t loop  = cpu->regs[VM_REG_A1];
    uint32_t status = 0, handle = 0;
    if (!audio_call(REQ_AUDIO_LOAD_MUSIC, intro, loop, cpu->vm_id, 0,
                    &status, &handle)) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? handle : 0;
}
/* ---- SYS_AUDIO_GET_LEVELS (out_buf, n_bands) -> bands written ----
 * Round-trips REQ_AUDIO_GET_LEVELS; the response packs up to 16 band
 * bytes into a0..a3 and the count in a4. Unpack into the guest buffer. */
static void handle_get_levels(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t guest_buf = cpu->regs[VM_REG_A0];
    uint32_t want      = cpu->regs[VM_REG_A1];
    if (want > 16) want = 16;

    ChannelMsg m;
    memset(&m, 0, sizeof(m));
    m.type = REQ_AUDIO_GET_LEVELS;
    if (!channel_request_call(g_channel, &m, g_timeout_ms, NULL, NULL)) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    uint32_t got = m.a4;
    if (got > want) got = want;
    if (got == 0) { cpu->regs[VM_REG_A0] = 0; return; }

    uint8_t *dst = (uint8_t *)vm_translate_write(cpu, guest_buf, got);
    if (!dst) { cpu->regs[VM_REG_A0] = 0; return; }

    uint32_t words[4] = { m.a0, m.a1, m.a2, m.a3 };
    for (uint32_t i = 0; i < got; i++)
        dst[i] = (uint8_t)((words[i >> 2] >> ((i & 3) * 8)) & 0xFFu);
    cpu->regs[VM_REG_A0] = got;
}

/* ---- SYS_AUDIO_LOAD_WAV (path) -> object handle ----
 * The guest passes a "/host/<name>.wav" path; the host resolves it to
 * <host_fs_root>/<name>, reads + parses the WAV, downmixes to mono
 * PCM16 directly into the shared staging buffer, and posts
 * REQ_AUDIO_LOAD_STAGED. Host-side parse keeps the work out of the
 * guest's small data region and reuses the host WAV parser. */
static void copy_guest_str(VmCpu *cpu, uint32_t addr, char *dst, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        const uint8_t *b = vm_translate_read(cpu, addr + (uint32_t)i, 1);
        if (!b || *b == 0) break;
        dst[i] = (char)*b;
    }
    dst[i] = 0;
}

static void handle_load_wav(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t path_addr = cpu->regs[VM_REG_A0];   /* read arg BEFORE clearing */
    cpu->regs[VM_REG_A0] = 0;                     /* default: failure */
    if (!g_host_fs_root || !g_staging) return;

    char gpath[256];
    copy_guest_str(cpu, path_addr, gpath, sizeof(gpath));

    /* Resolve "/host/<rest>" -> "<root>/<rest>". Only the /host mount
     * is served this way; reject anything else. */
    const char *rest = NULL;
    if (strncmp(gpath, "/host/", 6) == 0) rest = gpath + 6;
    else return;

    char hostpath[512];
    snprintf(hostpath, sizeof(hostpath), "%s/%s", g_host_fs_root, rest);

    FILE *fp = fopen(hostpath, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return; }
    uint8_t *filebuf = malloc((size_t)sz);
    if (!filebuf) { fclose(fp); return; }
    size_t rd = fread(filebuf, 1, (size_t)sz, fp);
    fclose(fp);

    WavInfo info;
    if (wav_parse(filebuf, rd, &info) != WAV_OK) { free(filebuf); return; }

    /* downmix straight into the staging buffer (mono int16) */
    uint32_t max_frames = (uint32_t)(g_staging_cap / sizeof(int16_t));
    uint32_t frames = wav_to_mono_pcm16(&info, (int16_t *)g_staging, max_frames);
    free(filebuf);
    if (frames == 0) return;

    uint32_t status = 0, handle = 0;
    if (!audio_call(REQ_AUDIO_LOAD_STAGED, frames * sizeof(int16_t),
                    cpu->vm_id, 0, 0, &status, &handle)) return;
    cpu->regs[VM_REG_A0] = (status == 0) ? handle : 0;
}
static void handle_fft_enable(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t en = cpu->regs[VM_REG_A0];
    uint32_t status = 0, h = 0;
    audio_call(REQ_AUDIO_FFT_ENABLE, en ? 1u : 0u, 0, 0, 0, &status, &h);
    cpu->regs[VM_REG_A0] = 0;
}

/* ---- installation ---- */
bool vm_host_install_audio(VmSystem *sys, const VmHostAudioConfig *cfg) {
    if (!sys || !sys->ecall_router || !cfg || !cfg->channel) return false;

    g_channel     = cfg->channel;
    g_staging     = (uint8_t *)cfg->staging_buffer;
    g_staging_cap = cfg->staging_capacity;
    g_timeout_ms  = cfg->call_timeout_ms ? cfg->call_timeout_ms : 1000u;
    g_host_fs_root = cfg->host_fs_root;

    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_LOAD_SAMPLE,
                           handle_load_sample)) goto fail;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_LOAD_MUSIC,
                           handle_load_music)) goto f1;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_FREE,
                           handle_free)) goto f2;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_TRIGGER_SFX,
                           handle_trigger_sfx)) goto f3;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_PLAY_MUSIC,
                           handle_play_music)) goto f4;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_STOP,
                           handle_stop)) goto f5;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_SET_GAIN,
                           handle_set_gain)) goto f6;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_GET_LEVELS,
                           handle_get_levels)) goto f7;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_FFT_ENABLE,
                           handle_fft_enable)) goto f8;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_LOAD_WAV,
                           handle_load_wav)) goto f9;
    return true;

f9: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_FFT_ENABLE);
f8: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_GET_LEVELS);
f7: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_SET_GAIN);
f6: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_STOP);
f5: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_PLAY_MUSIC);
f4: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_TRIGGER_SFX);
f3: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_FREE);
f2: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_LOAD_MUSIC);
f1: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_LOAD_SAMPLE);
fail:
    g_channel = NULL;
    return false;
}
