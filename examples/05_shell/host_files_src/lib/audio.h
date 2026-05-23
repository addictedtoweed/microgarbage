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

typedef uint32_t audio_object;   /* durable loaded sample/music */
typedef uint32_t audio_voice;    /* transient playing instance  */

#define AUDIO_OBJECT_NONE  0u
#define AUDIO_VOICE_NONE   0u

/* Load PCM (already in guest memory) into a pool object. `pcm` points
 * at `size` bytes of sample data (format the mixer expects — PCM16
 * mono in this build). Returns an object handle, or AUDIO_OBJECT_NONE
 * on failure (out of audio memory, too big for the staging buffer). */
static inline audio_object audio_load_sample(const void *pcm, uint32_t size) {
    return (audio_object)_vm_sys2(SYS_AUDIO_LOAD_SAMPLE,
                                  (uint32_t)pcm, size);
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
    return (audio_voice)_vm_sys3(SYS_AUDIO_TRIGGER_SFX, obj,
                                 (uint32_t)gain_q15, (uint32_t)pan_q15);
}

/* Play a music object (intro+loop). Returns a voice handle or
 * AUDIO_VOICE_NONE. (Music path lands with the next slice; for now
 * this returns NONE.) */
static inline audio_voice audio_play_music(audio_object obj, uint32_t flags) {
    return (audio_voice)_vm_sys2(SYS_AUDIO_PLAY_MUSIC, obj, flags);
}

/* Stop a playing voice. Returns 0 or a negative errno. */
static inline int audio_stop(audio_voice v) {
    return (int)_vm_sys1(SYS_AUDIO_STOP, v);
}

/* Set a playing voice's gain (q15). Returns 0 or negative errno. */
static inline int audio_set_gain(audio_voice v, int32_t gain_q15) {
    return (int)_vm_sys2(SYS_AUDIO_SET_GAIN, v, (uint32_t)gain_q15);
}

/* Fill `out` with up to `n_bands` FFT band levels for the meters.
 * Returns the number written (0 until the meter feature lands). */
static inline uint32_t audio_get_levels(uint8_t *out, uint32_t n_bands) {
    return _vm_sys2(SYS_AUDIO_GET_LEVELS, (uint32_t)out, n_bands);
}

#endif /* GUEST_AUDIO_H */
