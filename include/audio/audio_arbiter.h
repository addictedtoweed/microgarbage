/* ============================================================
 *  audio_arbiter.h — track arbitration + the two-handle model
 *
 *  Turns durable POOL OBJECT handles into transient VOICE handles,
 *  arbitrating a fixed pool of N mixer tracks (docs/audio-
 *  architecture.md).
 *
 *  ---------------------------------------------------------------
 *  Two handles, two lifetimes
 *  ---------------------------------------------------------------
 *    OBJECT handle (AudioObjHandle, from audio_pool):
 *        durable, cheap, pool-bounded. The loaded sample / music
 *        data. A VM may hold as many as it wants.
 *
 *    VOICE handle (AudioVoiceHandle, from here):
 *        transient, scarce, track-bounded. Returned by trigger/play.
 *        Valid only while that playback is live. There are only N
 *        tracks, so a play/trigger may be REJECTED when all are busy.
 *
 *  Playing an object claims a free track and returns a voice handle;
 *  the same object can be triggered many times, each getting (or
 *  being denied) its own voice. Stopping/finishing a voice frees its
 *  track.
 *
 *  ---------------------------------------------------------------
 *  Policy: FCFS, reject-on-full
 *  ---------------------------------------------------------------
 *  When all N tracks are busy, a new play/trigger returns the
 *  invalid voice handle (REJECTED). NOT evict-oldest (cutting a song
 *  for an SFX is worse than silence), NOT queue (late audio is worse
 *  than no audio). Priority eviction is a deferred future refinement;
 *  the AudioVoiceParams.priority field is reserved for it now but
 *  currently unused by the FCFS policy.
 *
 *  ---------------------------------------------------------------
 *  WIP: priority admission/eviction + mute states (safety equipment)
 *  ---------------------------------------------------------------
 *  This OS is intended to run in safety equipment, where the audio
 *  policy must GUARANTEE a warning can sound. None of the following is
 *  implemented yet — today it is strictly FCFS reject-on-full and
 *  `priority` is ignored — but it is the planned model:
 *    - Priority admission: when all tracks are busy, a higher-priority
 *      voice evicts the lowest-priority active voice instead of being
 *      blanket-REJECTED.
 *    - A highest-priority "warning/alarm" voice that always claims a
 *      track (preempting if needed), playable once or on repeat, and
 *      never reaped/ducked/muted by lower-priority audio.
 *    - Mute states: TEMPORARY mute (silence or duck, then resume the
 *      prior voices) and PERMANENT mute — with the warning priority
 *      overriding a temporary mute so alarms still play.
 *  When this lands, wire it into audio_arbiter_play (admission) and a
 *  new "play warning" entry point, plus mixer mute/duck control.
 *
 *  ---------------------------------------------------------------
 *  Refcount integration (lifetime safety)
 *  ---------------------------------------------------------------
 *  A live voice holds a reference on its pool object (audio_pool_ref
 *  on play, audio_pool_unref on stop). So a playing object cannot be
 *  freed out from under the mixer even if its creator VM frees its
 *  own handle — the object survives until the last voice stops. This
 *  is the cross-VM lifetime model from the design note, realized.
 *
 *  ---------------------------------------------------------------
 *  Mixer seam
 *  ---------------------------------------------------------------
 *  The arbiter owns voice/track POLICY and bookkeeping; it does not
 *  itself mix. It drives playback through a caller-supplied
 *  AudioArbiterSink: "start object O on track T as voice V" and
 *  "stop track T". On the desktop/MCU this sink wires to the real
 *  mixer + music players (step 6); in tests it's a stub that records
 *  calls. Keeping the sink behind a vtable keeps the arbiter pure and
 *  unit-testable.
 *
 *  ---------------------------------------------------------------
 *  Concurrency
 *  ---------------------------------------------------------------
 *  Like the pool, the arbiter is single-context (service side, M4 /
 *  worker thread), mutated only by serialized channel requests. Not
 *  internally locked; do not call from two threads.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_ARBITER_H
#define AUDIO_ARBITER_H

#include "audio/audio_pool.h"

#include <stdint.h>
#include <stdbool.h>

#ifndef AUDIO_ARBITER_MAX_TRACKS
#define AUDIO_ARBITER_MAX_TRACKS  16u
#endif

#define AUDIO_VOICE_NONE  0u

typedef uint32_t AudioVoiceHandle;   /* packed (generation<<16 | track+1) */

typedef enum {
    AUDIO_ARB_OK = 0,
    AUDIO_ARB_REJECTED,        /* all tracks busy (FCFS, reject-on-full) */
    AUDIO_ARB_BAD_OBJECT,      /* object handle invalid                  */
    AUDIO_ARB_BAD_VOICE,       /* voice handle invalid / stale           */
    AUDIO_ARB_INVALID_ARG,
} AudioArbResult;

/* What kind of playback a voice represents. */
typedef enum {
    AUDIO_VOICE_SFX = 0,       /* one-shot sample                        */
    AUDIO_VOICE_MUSIC,         /* intro+loop music object                */
} AudioVoiceKind;

/* Per-voice parameters at trigger time. */
typedef struct {
    int32_t  gain;             /* q15 volume (Q15_ONE = unity)           */
    int32_t  pan;              /* q15 pan (-1..+1)                       */
    uint8_t  priority;         /* WIP: reserved for priority admission/
                                * eviction (see policy note above);
                                * IGNORED by today's FCFS policy         */
    uint8_t  loop;             /* SFX: loop forever (music loops via its
                                * own intro/loop machinery)              */
} AudioVoiceParams;

/* The playback sink the arbiter drives. The arbiter calls start when
 * a voice claims a track and stop when it frees one. Implementations
 * wire these to the real mixer/music players; tests stub them.
 * `start` returns true if playback actually began (false makes the
 * arbiter abort the claim and free the track). */
typedef struct {
    bool (*start)(void *ctx, uint32_t track, AudioVoiceKind kind,
                  AudioObjHandle object, const AudioVoiceParams *params);
    void (*stop)(void *ctx, uint32_t track);
    void *ctx;
    /* Optional. Returns true if the one-shot voice playing on `track`
     * has finished (e.g. its mixer channel drained). Lets
     * audio_arbiter_reap() free finished SFX tracks. May be NULL — then
     * the arbiter never auto-reaps and SFX voices free only via stop /
     * sweep. Placed after ctx so existing positional sink initializers
     * (start, stop, ctx) keep compiling with is_done == NULL. */
    bool (*is_done)(void *ctx, uint32_t track);
} AudioArbiterSink;

/* One track's state. */
typedef struct {
    bool             active;
    AudioObjHandle   object;     /* pool object this voice plays         */
    AudioVoiceKind   kind;
    uint16_t         owner_vm;   /* for sweep-on-VM-death                */
    uint16_t         generation; /* bumped on free; matched vs handle    */
} AudioTrack;

typedef struct {
    AudioPool        *pool;      /* for ref/unref on the played objects  */
    AudioArbiterSink  sink;
    AudioTrack        tracks[AUDIO_ARBITER_MAX_TRACKS];
    uint16_t          generations[AUDIO_ARBITER_MAX_TRACKS];
    uint32_t          track_count;   /* <= MAX_TRACKS                    */
    uint32_t          active_count;
} AudioArbiter;

/* ---- lifecycle ---- */

/* Initialize with `track_count` tracks (clamped to MAX_TRACKS), a
 * pool (for object ref/unref), and a sink (copied in). Returns false
 * on bad args. */
bool audio_arbiter_init(AudioArbiter *a, uint32_t track_count,
                        AudioPool *pool, const AudioArbiterSink *sink);

/* ---- play / stop ---- */

/* Play an object as a voice. Claims a free track (FCFS); on success
 * takes a pool ref on the object, calls sink.start, and returns the
 * voice handle via *out_voice. Returns AUDIO_ARB_REJECTED (and
 * *out_voice = AUDIO_VOICE_NONE) if all tracks are busy, or
 * AUDIO_ARB_BAD_OBJECT if the object handle is invalid. */
AudioArbResult audio_arbiter_play(AudioArbiter *a,
                                  AudioObjHandle object,
                                  AudioVoiceKind kind,
                                  const AudioVoiceParams *params,
                                  uint16_t owner_vm,
                                  AudioVoiceHandle *out_voice);

/* Play a voice that has NO backing pool object — the service drives
 * the audio itself (e.g. a file-stream music voice reading off SD).
 * Claims a free track (FCFS) and calls sink.start with
 * object = AUDIO_POOL_HANDLE_NONE; takes no pool ref. `kind` is forced
 * to MUSIC so reap never touches it (it loops until stopped/swept).
 * Returns AUDIO_ARB_REJECTED if all tracks are busy. */
AudioArbResult audio_arbiter_play_external(AudioArbiter *a,
                                           const AudioVoiceParams *params,
                                           uint16_t owner_vm,
                                           AudioVoiceHandle *out_voice);

/* Stop a voice: calls sink.stop, drops the pool ref, frees the track
 * (bumps its generation so the voice handle goes stale). Returns
 * AUDIO_ARB_BAD_VOICE if the handle is invalid/stale (e.g. the voice
 * already finished). */
AudioArbResult audio_arbiter_stop(AudioArbiter *a, AudioVoiceHandle voice);

/* Called when a voice finishes on its own (e.g. a one-shot SFX ran
 * out). Same effect as stop but expresses "playback ended" intent.
 * The sink's stop is still invoked (idempotent cleanup). */
AudioArbResult audio_arbiter_voice_finished(AudioArbiter *a,
                                            AudioVoiceHandle voice);

/* Free any one-shot (SFX) voices whose playback has finished, as
 * reported by sink.is_done. Music voices are NEVER reaped here (they
 * loop until explicitly stopped/swept). Returns the count reaped. Call
 * this each service cycle — without it, one-shot SFX tracks are never
 * reclaimed and the arbiter fills up (every new trigger REJECTED).
 * No-op if the sink provides no is_done callback. */
uint32_t audio_arbiter_reap(AudioArbiter *a);

/* Stop all voices owned by a dying VM (drops their pool refs, frees
 * their tracks). Returns the number stopped via *out_stopped (may be
 * NULL). */
AudioArbResult audio_arbiter_sweep_vm(AudioArbiter *a, uint16_t owner_vm,
                                      uint32_t *out_stopped);

/* ---- introspection ---- */

bool     audio_arbiter_voice_valid(const AudioArbiter *a, AudioVoiceHandle v);
uint32_t audio_arbiter_active_count(const AudioArbiter *a);
uint32_t audio_arbiter_free_tracks(const AudioArbiter *a);
/* The pool object a voice is playing, or AUDIO_POOL_HANDLE_NONE. */
AudioObjHandle audio_arbiter_voice_object(const AudioArbiter *a,
                                          AudioVoiceHandle v);

#endif /* AUDIO_ARBITER_H */
