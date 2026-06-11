/* ============================================================
 *  mg_audio.c — guest-side audio ecall stubs.
 *  See mg_audio.h for the contract.
 *
 *  Every function is a thin rename over the existing SYS_AUDIO_*
 *  ecalls (1160-1170 in vm_ecall.h). The arbiter, pool, and stream
 *  machinery were already in place from the existing audio service —
 *  this file is purely an ergonomic wrapper layer for cart-side
 *  games.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_audio.h"
#include "vm_runtime.h"

MgSfx mg_sfx_load(const char *path) {
    return (MgSfx)(int32_t)_vm_sys1(SYS_AUDIO_LOAD_WAV, (uint32_t)path);
}

MgSfx mg_sfx_load_sample(const void *buf, uint32_t bytes) {
    return (MgSfx)(int32_t)_vm_sys2(SYS_AUDIO_LOAD_SAMPLE,
                                    (uint32_t)buf, bytes);
}

void mg_sfx_free(MgSfx handle) {
    (void)_vm_sys1(SYS_AUDIO_FREE, (uint32_t)handle);
}

MgVoice mg_sfx_play(MgSfx handle, int16_t gain_q15, int16_t pan_q15) {
    return (MgVoice)(int32_t)_vm_sys3(SYS_AUDIO_TRIGGER_SFX,
                                      (uint32_t)handle,
                                      (uint32_t)(uint16_t)gain_q15,
                                      (uint32_t)(uint16_t)pan_q15);
}

MgVoice mg_stream_play(const char *path, uint32_t flags) {
    /* SYS_AUDIO_STREAM_WAV historically took only (path). We pass
     * flags as the second arg; if the host handler ignores it (older
     * impl), looping just doesn't engage. As the handler is
     * extended for MG_AUDIO_LOOP, the bit goes through cleanly. */
    return (MgVoice)(int32_t)_vm_sys2(SYS_AUDIO_STREAM_WAV,
                                      (uint32_t)path, flags);
}

MgVoice mg_audio_pcm_stream_open(uint32_t sample_rate_hz) {
    /* channels = 2 (stereo) is the only supported value in v2.02. */
    return (MgVoice)(int32_t)_vm_sys2(SYS_AUDIO_PCM_STREAM_OPEN,
                                       sample_rate_hz, 2u);
}

uint32_t mg_audio_pcm_stream_feed(MgVoice voice,
                                   const int16_t *interleaved_stereo,
                                   uint32_t frame_count) {
    return (uint32_t)_vm_sys3(SYS_AUDIO_PCM_STREAM_FEED,
                              (uint32_t)voice,
                              (uint32_t)interleaved_stereo,
                              frame_count);
}

void mg_audio_pcm_stream_close(MgVoice voice) {
    (void)_vm_sys1(SYS_AUDIO_PCM_STREAM_CLOSE, (uint32_t)voice);
}

void mg_audio_stop(MgVoice voice) {
    (void)_vm_sys1(SYS_AUDIO_STOP, (uint32_t)voice);
}

void mg_audio_gain(MgVoice voice, int16_t gain_q15) {
    (void)_vm_sys2(SYS_AUDIO_SET_GAIN,
                   (uint32_t)voice,
                   (uint32_t)(uint16_t)gain_q15);
}
