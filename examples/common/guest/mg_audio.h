/* ============================================================
 *  mg_audio.h — sound effects + streaming music for cart-side games.
 *
 *  Thin renames over the existing SYS_AUDIO_* ecalls (1160-1170).
 *  The audio_arbiter handles admission and eviction automatically;
 *  mg_sfx_play / mg_stream_play return MG_VOICE_REJECTED (0) when
 *  the arbiter couldn't fit the request, which lets game code
 *  decide whether to retry, fall back to a quieter SFX, etc.
 *
 *  Handles auto-free on VM exit via the existing
 *  audio_service_sweep_vm(vm_id) — game code doesn't need to call
 *  mg_sfx_free explicitly at shutdown.
 *
 *  See docs/game-api.md for design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_AUDIO_H
#define MG_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loaded sound effect / sample handle. 0 means "load failed". */
typedef int32_t MgSfx;

/* Active voice handle from a play/trigger/stream call. 0 means
 * "rejected by arbiter" (no eviction won, slot too low priority). */
typedef int32_t MgVoice;

#define MG_VOICE_REJECTED   ((MgVoice)0)

/* Bit flags for mg_stream_play(). */
#define MG_AUDIO_LOOP       (1u << 0)

/* -------- SFX (short, fully resident) -------- */

/* Load a .wav from path (`/cart/foo.wav` or `/host/...`). Returns an
 * MgSfx handle for later mg_sfx_play / mg_sfx_free, or 0 if the load
 * failed (file missing, malformed, audio pool out of space). */
MgSfx   mg_sfx_load       (const char *path);

/* Load a sample from a guest-side buffer (PCM 16-bit mono / stereo
 * matching the audio service's configured sample rate). */
MgSfx   mg_sfx_load_sample(const void *buf, uint32_t bytes);

/* Release the handle's pool ref. Auto-called on VM exit by the
 * service's sweep hook; explicit free is optional. */
void    mg_sfx_free       (MgSfx handle);

/* Attempt to trigger playback. The arbiter may evict a lower-priority
 * voice to make room; returns the new voice handle or
 * MG_VOICE_REJECTED if the request lost. gain/pan are Q15 (32767 =
 * 1.0; pan -32767 = full left, 32767 = full right). */
MgVoice mg_sfx_play       (MgSfx handle, int16_t gain_q15, int16_t pan_q15);

/* -------- Streaming music -------- */

/* Open + prime + play a streaming WAV from path. `flags` is a
 * bitmask: MG_AUDIO_LOOP for seamless looping. Returns a voice
 * handle or MG_VOICE_REJECTED if a stream slot couldn't be reserved. */
MgVoice mg_stream_play    (const char *path, uint32_t flags);

/* -------- Voice control -------- */

/* Stop and release the voice. After this the handle is dead. */
void    mg_audio_stop     (MgVoice voice);

/* Update a playing voice's gain. Useful for fades. */
void    mg_audio_gain     (MgVoice voice, int16_t gain_q15);

#ifdef __cplusplus
}
#endif

#endif /* MG_AUDIO_H */
