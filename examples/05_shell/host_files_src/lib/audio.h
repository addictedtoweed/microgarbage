/* ============================================================
 *  audio.h — guest-side audio API for shell apps
 *
 *  Thin wrappers over the SYS_AUDIO_* ecalls. A guest loads samples
 *  (getting durable OBJECT handles), then triggers them (getting
 *  transient VOICE handles). Audio is a shared service; the host
 *  stamps this VM as owner so everything is cleaned up if the app
 *  exits.
 *
 *  Q15 gain/pan: 32767 == unity / center. Pan -32768=left, 0=center,
 *  +32767=right.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GUEST_AUDIO_H
#define GUEST_AUDIO_H

#include "vm_runtime.h"
#include <stdint.h>
#include <stddef.h>

#define AUDIO_GAIN_UNITY   32767
#define AUDIO_PAN_CENTER   0

/* Largest plausible meter band count; anything above this from a
 * GET_LEVELS call is a negative errno (audio unavailable), not a real
 * count. (The service uses 16 bands; allow headroom.) */
#define AUDIO_FFT_BANDS_MAX 64

/* A syscall return is a failure if it's a small negative errno code.
 * The host writes negative errno (e.g. -ENOSYS = -38 = 0xFFFFFFDA) for
 * unknown/failed syscalls. We must NOT mistake a valid music handle —
 * which is high-bit *tagged* (0x80……) and so also "negative" as
 * int32 — for an error. Linux errnos are small (1..4095), so only
 * returns in the top 4096 codes (0xFFFFF001..0xFFFFFFFF, i.e. -1..-4095)
 * count as failures; tagged handles (0x80000001..0xBFFFFFFF) do not. */
static inline int _audio_failed(uint32_t r) {
    return r >= 0xFFFFF001u;     /* -1 .. -4095 */
}

typedef uint32_t audio_object;   /* durable loaded sample/music */
typedef uint32_t audio_voice;    /* transient playing instance  */

#define AUDIO_OBJECT_NONE  0u
#define AUDIO_VOICE_NONE   0u

/* Load PCM (already in guest memory) into a pool object. `pcm` points
 * at `size` bytes of sample data (format the mixer expects — PCM16
 * mono in this build). Returns an object handle, or AUDIO_OBJECT_NONE
 * on failure (out of audio memory, too big for the staging buffer). */
static inline audio_object audio_load_sample(const void *pcm, uint32_t size) {
    uint32_t r = _vm_sys2(SYS_AUDIO_LOAD_SAMPLE, (uint32_t)pcm, size);
    return _audio_failed(r) ? AUDIO_OBJECT_NONE : (audio_object)r;
}

/* Release the caller's reference to an object. The object survives
 * while any voice still plays it (refcounted), so this is safe to
 * call right after triggering. */
static inline void audio_free(audio_object obj) {
    (void)_vm_sys1(SYS_AUDIO_FREE, obj);
}

/* Trigger a one-shot SFX. Returns a voice handle, or AUDIO_VOICE_NONE
 * if all tracks are busy (REJECTED) or the object is invalid. */
static inline audio_voice audio_trigger(audio_object obj,
                                        int32_t gain_q15, int32_t pan_q15) {
    uint32_t r = _vm_sys3(SYS_AUDIO_TRIGGER_SFX, obj,
                          (uint32_t)gain_q15, (uint32_t)pan_q15);
    return _audio_failed(r) ? AUDIO_VOICE_NONE : (audio_voice)r;
}

/* Pair two already-loaded sample objects (intro + loop) into a music
 * object. Pass AUDIO_OBJECT_NONE for loop for an intro-only object.
 * Returns a (tagged) music handle, or AUDIO_OBJECT_NONE. The music
 * object holds its own refs on intro/loop, so the caller may free its
 * own refs to them afterward. */
static inline audio_object audio_load_music(audio_object intro,
                                            audio_object loop) {
    uint32_t r = _vm_sys2(SYS_AUDIO_LOAD_MUSIC, intro, loop);
    return _audio_failed(r) ? AUDIO_OBJECT_NONE : (audio_object)r;
}

/* Play a music object (intro+loop). Returns a voice handle or
 * AUDIO_VOICE_NONE (REJECTED if no music stream slot is free). */
static inline audio_voice audio_play_music(audio_object music, uint32_t flags) {
    uint32_t r = _vm_sys2(SYS_AUDIO_PLAY_MUSIC, music, flags);
    return _audio_failed(r) ? AUDIO_VOICE_NONE : (audio_voice)r;
}

/* Stop a playing voice. Returns 0 or a negative errno. */
static inline int audio_stop(audio_voice v) {
    return (int)_vm_sys1(SYS_AUDIO_STOP, v);
}

/* Set a playing voice's gain (q15). Returns 0 or negative errno. */
static inline int audio_set_gain(audio_voice v, int32_t gain_q15) {
    return (int)_vm_sys2(SYS_AUDIO_SET_GAIN, v, (uint32_t)gain_q15);
}

/* Enable or disable the band meter (the FFT over the mixed output).
 * Disabled by default; enable before reading levels. */
static inline void audio_fft_enable(int enable) {
    (void)_vm_sys1(SYS_AUDIO_FFT_ENABLE, enable ? 1u : 0u);
}

/* Fill `out` with up to `n_bands` FFT band levels (0..255) for the
 * meters. Returns the number written (0 if the meter is disabled or
 * audio is unavailable). Enable the meter first with
 * audio_fft_enable(1). Guards against the host returning a negative
 * errno (e.g. -ENOSYS when audio isn't installed): such a value, read
 * as unsigned, would be a huge bogus count — clamp it to 0. */
static inline uint32_t audio_get_levels(uint8_t *out, uint32_t n_bands) {
    uint32_t r = _vm_sys2(SYS_AUDIO_GET_LEVELS, (uint32_t)out, n_bands);
    if (r > n_bands) return 0;   /* negative errno or impossible count */
    return r;
}

/* True if the audio service is actually wired up on this host. Probes
 * with a harmless call; if it returns a negative errno (-ENOSYS, read
 * as a huge unsigned), audio isn't available (e.g. native-Windows
 * builds without the win32 channel transport). Apps should check this
 * before relying on audio. */
static inline int audio_available(void) {
    uint32_t r = _vm_sys2(SYS_AUDIO_GET_LEVELS, 0, 0);
    return !_audio_failed(r) && r <= AUDIO_FFT_BANDS_MAX;
}

/* ============================================================
 *  Drop-in .wav asset loader (host-side parse)
 *
 *  Load a PCM .wav file (e.g. dropped into /host) as an audio object.
 *  The HOST opens + parses the file and stages the PCM — the guest
 *  only passes a path. This keeps the work out of the guest's small
 *  (64 KB) data region: the host has megabytes and already has the
 *  WAV parser. Accepts 8/16-bit, mono/stereo PCM WAV; downmixed to
 *  mono PCM16 (the current SFX format).
 *
 *  Returns an object handle, or AUDIO_OBJECT_NONE on any error
 *  (missing file, not PCM, too big for the host staging buffer).
 * ============================================================ */
static inline audio_object audio_load_wav(const char *path) {
    uint32_t r = _vm_sys1(SYS_AUDIO_LOAD_WAV, (uint32_t)path);
    return _audio_failed(r) ? AUDIO_OBJECT_NONE : (audio_object)r;
}

#endif /* GUEST_AUDIO_H */
