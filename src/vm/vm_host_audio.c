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
#include "config.h"             /* GARBAGE_SCHED_MODE */
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

/* Serialize the VM->service round-trip. The audio channel is lock-free
 * SPSC (exactly one producer + one consumer); under preemption each VM
 * runs in its OWN thread, so two audio-using VMs would otherwise be
 * multiple producers on the request ring / consumers on the response
 * ring AND race on the shared staging buffer. Hold this across the whole
 * post+wait — and, for staged loads, across the staging write too — so
 * each exchange is atomic per VM. Cooperative builds step VMs serially
 * on one thread, so this compiles to nothing. (Mirrors fs_lock in
 * vm_host_fs.c.) */
#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE
#include <pthread.h>
static pthread_mutex_t g_audio_mtx = PTHREAD_MUTEX_INITIALIZER;
static inline void audio_lock(void)   { pthread_mutex_lock(&g_audio_mtx); }
static inline void audio_unlock(void) { pthread_mutex_unlock(&g_audio_mtx); }
#else
static inline void audio_lock(void)   { }
static inline void audio_unlock(void) { }
#endif

/* Post a request and wait for its response; returns the response's
 * status (a3) and handle (a4) via out params. Returns false on channel
 * failure / timeout. The _locked form assumes the audio lock is already
 * held (used when the caller also wrote the staging buffer under it). */
static bool audio_call_locked(uint16_t type, uint32_t a0, uint32_t a1,
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

static bool audio_call(uint16_t type, uint32_t a0, uint32_t a1,
                       uint32_t a2, uint32_t a3,
                       uint32_t *out_status, uint32_t *out_handle) {
    audio_lock();
    bool r = audio_call_locked(type, a0, a1, a2, a3, out_status, out_handle);
    audio_unlock();
    return r;
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

    /* Stage the PCM, then ask the service to copy staging->pool. Hold the
     * audio lock across the staging write + round-trip so a concurrent VM
     * can't clobber the shared staging buffer mid-flight. */
    audio_lock();
    memcpy(g_staging, src, size);
    uint32_t status = 0, handle = 0;
    bool ok = audio_call_locked(REQ_AUDIO_LOAD_STAGED, size, cpu->vm_id, 0, 0,
                                &status, &handle);
    audio_unlock();
    cpu->regs[VM_REG_A0] = (ok && status == 0) ? handle : 0;
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
    audio_lock();
    bool ok = channel_request_call(g_channel, &m, g_timeout_ms, NULL, NULL);
    audio_unlock();
    if (!ok) { cpu->regs[VM_REG_A0] = 0; return; }
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

/* Read + parse a WAV at an absolute host path, stage it as native-rate mono
 * PCM16, and load it into the audio pool owned by `owner_vm` (0 = host-owned).
 * Returns the pool object handle, or 0 on any failure. Shared by the guest
 * SYS_AUDIO_LOAD_WAV ecall and the host-side vm_host_audio_sfx_load. */
static uint32_t load_wav_to_pool(const char *hostpath, uint16_t owner_vm) {
    if (!g_staging || !hostpath) return 0;

    FILE *fp = fopen(hostpath, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return 0; }
    uint8_t *filebuf = malloc((size_t)sz);
    if (!filebuf) { fclose(fp); return 0; }
    size_t rd = fread(filebuf, 1, (size_t)sz, fp);
    fclose(fp);

    WavInfo info;
    if (wav_parse(filebuf, rd, &info) != WAV_OK) { free(filebuf); return 0; }

    /* Stage at the WAV's NATIVE rate; the mixer interpolates per-channel at
     * play time (a2 of LOAD_STAGED carries the rate). */
    uint32_t max_frames = (uint32_t)(g_staging_cap / sizeof(int16_t));
    audio_lock();
    uint32_t frames = wav_to_mono_pcm16(&info, (int16_t *)g_staging, max_frames);
    free(filebuf);
    if (frames == 0) { audio_unlock(); return 0; }
    uint32_t status = 0, handle = 0;
    bool ok = audio_call_locked(REQ_AUDIO_LOAD_STAGED, frames * sizeof(int16_t),
                                owner_vm, info.sample_rate, 0, &status, &handle);
    audio_unlock();
    return (ok && status == 0) ? handle : 0;
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
    cpu->regs[VM_REG_A0] = load_wav_to_pool(hostpath, cpu->vm_id);
}
/* ---- SYS_AUDIO_STREAM_WAV (path) -> voice handle or 0 ----
 * Unlike LOAD_WAV (which decodes the whole file into the pool), this
 * streams an arbitrarily long WAV: the host only RESOLVES the guest
 * mount path to a native path the service's file_reader understands,
 * stages that path string, and posts REQ_AUDIO_STREAM_WAV. The service
 * opens + reads the file incrementally (on the desktop worker, or the
 * H745 M4 off SD). "/host/x" -> "<root>/x" (stdio); "/td0/x" ->
 * "td0:/x" (the trashfs RAM disk). */
static void handle_stream_wav(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t path_addr = cpu->regs[VM_REG_A0];   /* read BEFORE clearing */
    cpu->regs[VM_REG_A0] = 0;                     /* default: failure */
    if (!g_staging) return;

    char gpath[256];
    copy_guest_str(cpu, path_addr, gpath, sizeof(gpath));

    char native[512];
    if (strncmp(gpath, "/host/", 6) == 0) {
        if (!g_host_fs_root) return;
        snprintf(native, sizeof(native), "%s/%s", g_host_fs_root, gpath + 6);
    } else if (strncmp(gpath, "/td0/", 5) == 0) {
        snprintf(native, sizeof(native), "td0:/%s", gpath + 5); /* trashfs */
    } else {
        return;   /* only /host and /td0 are streamable */
    }

    size_t plen = strlen(native) + 1;             /* include the NUL */
    if (plen > g_staging_cap) return;
    audio_lock();
    memcpy(g_staging, native, plen);
    uint32_t status = 0, voice = 0;
    bool ok = audio_call_locked(REQ_AUDIO_STREAM_WAV, (uint32_t)plen,
                                cpu->vm_id, 0, 0, &status, &voice);
    audio_unlock();
    cpu->regs[VM_REG_A0] = (ok && status == 0) ? voice : 0;
}

/* ---- SYS_AUDIO_PCM_STREAM_OPEN (rate, channels) -> voice or 0 ----
 * Opens a guest-fed streaming voice for raw int16 stereo audio. v2.02
 * accepts channels=2 only (caller duplicates mono → L=R before feeding).
 * The mixer's per-channel source-rate interpolator handles any rate
 * mismatch with the mixer output rate. Used by demo_fmv for the
 * per-FMV-frame audio chunk path. */
static void handle_pcm_stream_open(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t rate     = cpu->regs[VM_REG_A0];
    uint32_t channels = cpu->regs[VM_REG_A1];
    uint32_t status = 0, voice = 0;
    if (!audio_call(REQ_AUDIO_PCM_STREAM_OPEN, rate, channels, cpu->vm_id,
                    0, &status, &voice)) {
        cpu->regs[VM_REG_A0] = 0; return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? voice : 0;
}

/* ---- SYS_AUDIO_PCM_STREAM_FEED (voice, frames_buf, frame_count)
 *      -> frames_fed (0..frame_count) ----
 *
 * Copies guest's int16 stereo PCM into the shared staging buffer, then
 * asks the service to push it into the voice's mixer channel ring.
 * Returns the actual frame count fed; the caller retries for the
 * remainder if it's less than requested (channel ring was full). */
static void handle_pcm_stream_feed(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t voice       = cpu->regs[VM_REG_A0];
    uint32_t frames_buf  = cpu->regs[VM_REG_A1];
    uint32_t frame_count = cpu->regs[VM_REG_A2];
    cpu->regs[VM_REG_A0] = 0;

    if (!g_staging || frame_count == 0) return;
    /* int16 stereo = 4 bytes/frame */
    uint64_t bytes = (uint64_t)frame_count * 4u;
    if (bytes == 0 || bytes > g_staging_cap) return;

    const void *src = vm_translate_read(cpu, frames_buf, (uint32_t)bytes);
    if (!src) return;

    audio_lock();
    memcpy(g_staging, src, (size_t)bytes);
    uint32_t status = 0, fed = 0;
    bool ok = audio_call_locked(REQ_AUDIO_PCM_STREAM_FEED,
                                voice, frame_count, 0, 0, &status, &fed);
    audio_unlock();
    cpu->regs[VM_REG_A0] = (ok && status == 0) ? fed : 0;
}

/* ---- SYS_AUDIO_PCM_STREAM_CLOSE (voice) -> 0 or -errno ---- */
static void handle_pcm_stream_close(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t voice = cpu->regs[VM_REG_A0];
    uint32_t status = 0, h = 0;
    if (!audio_call(REQ_AUDIO_PCM_STREAM_CLOSE, voice, 0, 0, 0, &status, &h)) {
        cpu->regs[VM_REG_A0] = (uint32_t)(-VM_EIO); return;
    }
    cpu->regs[VM_REG_A0] = (status == 0) ? 0u : (uint32_t)(-VM_EINVAL);
}

static void handle_fft_enable(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t en = cpu->regs[VM_REG_A0];
    uint32_t status = 0, h = 0;
    /* a1 = owner_vm so the service can refcount the meter per consumer
     * and release this VM's hold on sweep. */
    audio_call(REQ_AUDIO_FFT_ENABLE, en ? 1u : 0u, cpu->vm_id, 0, 0, &status, &h);
    cpu->regs[VM_REG_A0] = 0;
}

/* ---- teardown reclaim ---- */
void vm_host_audio_sweep_vm(uint16_t vm_id) {
    if (!g_channel) return;   /* audio never installed */
    uint32_t status = 0, h = 0;
    audio_call(REQ_AUDIO_SWEEP_VM, vm_id, 0, 0, 0, &status, &h);
}

/* ---- Host-driven FMV clip audio (#73, no VM) — the FMV player opens/closes a
 * music_player voice fed by the audio service's fmv_ring (the FMV video
 * producer pushes the clip's audio into that ring directly). owner_vm = 0
 * (host-owned); the player closes it explicitly. */
uint32_t vm_host_audio_fmv_open(void) {
    if (!g_channel) return 0;
    uint32_t status = 0, voice = 0;
    if (!audio_call(REQ_AUDIO_FMV_OPEN, 0u, 0u, 0u, 0u, &status, &voice))
        return 0;
    return (status == 0) ? voice : 0;
}

void vm_host_audio_fmv_close(uint32_t voice) {
    if (!g_channel) return;
    uint32_t status = 0, h = 0;
    audio_call(REQ_AUDIO_FMV_CLOSE, voice, 0u, 0u, 0u, &status, &h);
}

uint32_t vm_host_audio_sfx_load(const char *host_relname) {
    if (!g_host_fs_root || !host_relname) return 0;
    char hostpath[512];
    snprintf(hostpath, sizeof(hostpath), "%s/%s", g_host_fs_root, host_relname);
    return load_wav_to_pool(hostpath, 0u);   /* host-owned (vm 0) */
}

uint32_t vm_host_audio_sfx_trigger(uint32_t obj, uint32_t gain_q15, int32_t pan_q15) {
    if (!g_channel || !obj) return 0;
    uint32_t status = 0, voice = 0;
    if (!audio_call(REQ_AUDIO_TRIGGER_SFX, obj, gain_q15, (uint32_t)pan_q15, 0u,
                    &status, &voice))
        return 0;
    return (status == 0) ? voice : 0;   /* 0 = no free track / rejected */
}

/* Adapter so the sweep can ride vm_system's unload-hook seam (called
 * before the VM's CPU/regions are freed). Idempotent + safe even if the
 * VM never touched audio (the service ignores an unknown vm_id). */
static void audio_unload_hook(uint16_t vm_id, void *userdata) {
    (void)userdata;
    vm_host_audio_sweep_vm(vm_id);
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
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_STREAM_WAV,
                           handle_stream_wav)) goto f10;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_PCM_STREAM_OPEN,
                           handle_pcm_stream_open)) goto f11;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_PCM_STREAM_FEED,
                           handle_pcm_stream_feed)) goto f12;
    if (!vm_ecall_register(sys->ecall_router, SYS_AUDIO_PCM_STREAM_CLOSE,
                           handle_pcm_stream_close)) goto f13;

    /* Reclaim a guest's audio resources (+ its FFT hold) when it exits
     * or crashes, before its vm_id can be reused. Best-effort: a full
     * hook table just means no auto-sweep (resources reclaim when the
     * service is torn down). */
    vm_system_register_unload_hook(sys, audio_unload_hook, NULL);
    return true;

f13: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_PCM_STREAM_CLOSE);
f12: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_PCM_STREAM_FEED);
f11: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_PCM_STREAM_OPEN);
f10: vm_ecall_unregister(sys->ecall_router, SYS_AUDIO_LOAD_WAV);
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
