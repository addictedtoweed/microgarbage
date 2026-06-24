/* ============================================================
 *  music_player.h — intro-and-loop music playback driver
 *
 *  Sits on top of audio_mixer. Manages a piece of music with an
 *  optional intro section that plays once, followed by a looping
 *  body that plays until stopped. The transition between intro
 *  and loop is sample-accurate and gap-free.
 *
 *  ---------------------------------------------------------------
 *  Architecture
 *  ---------------------------------------------------------------
 *
 *  The music player borrows a single mixer channel and feeds it
 *  samples over time. It pulls source data from the application
 *  through a stream callback — the application owns the SD card
 *  (or PSRAM, or whatever the source is) and the player just
 *  asks for samples by stream_id and offset.
 *
 *  Data flow:
 *
 *      SD card / PSRAM
 *           ↓ (app's stream_fn, sync, non-RT context)
 *      music_player.streaming_buffer (large, app-sized)
 *           ↓ (music_update, sync, non-RT context)
 *      mixer channel ring buffer (small, mixer-internal)
 *           ↓ (mixer_render, real-time context)
 *      output buffer
 *
 *  Pinned heads:
 *
 *      first N samples of intro → pinned RAM buffer
 *      first M samples of loop  → pinned RAM buffer
 *
 *  When (re)starting playback of a segment, the pinned head is
 *  played first while the streaming source catches up. This is
 *  what makes rapid-restart from "stop" instantaneous regardless
 *  of SD card latency.
 *
 *  ---------------------------------------------------------------
 *  Stream callback
 *  ---------------------------------------------------------------
 *
 *  The application provides one function that the player calls
 *  when it needs samples:
 *
 *      typedef size_t (*music_stream_fn)(
 *          void   *user_data,
 *          int     stream_id,
 *          void   *destination,
 *          size_t  requested_samples,
 *          size_t  offset_in_stream
 *      );
 *
 *  - `user_data` is opaque; passed through from config.
 *  - `stream_id` is opaque; player passes through the intro_stream_id
 *    or loop_stream_id from config so the callback knows which file.
 *  - `destination` is where to write the sample bytes.
 *  - `requested_samples` is how many samples (frames for stereo).
 *  - `offset_in_stream` is the sample offset within this stream.
 *  - Returns the number of samples actually provided (0..requested).
 *
 *  Returning fewer samples than requested signals end-of-stream.
 *  For the intro: triggers transition to loop.
 *  For the loop: triggers wrap to offset 0 (or pinned loop head).
 *
 *  If the application has a transient error (SD card busy, etc.),
 *  it should return `requested_samples` of silence rather than 0
 *  to avoid spurious end-of-stream signals.
 *
 *  The callback is called synchronously from music_update, which
 *  must run from a non-RT context (main loop or low-priority task).
 *
 *  ---------------------------------------------------------------
 *  Stream IDs
 *  ---------------------------------------------------------------
 *
 *  Stream IDs are opaque integers chosen by the application. A
 *  typical setup uses one stream_id per file, mapped by the
 *  callback to an open FILE* (or a trashfs file, or memory pointer).
 *  Use MUSIC_STREAM_NONE to indicate "no intro" or "no loop"
 *  (whichever is missing). At least one must be configured.
 *
 *  SD-card performance note: alternating between stream IDs forces
 *  the controller to track multiple positions and can cause seek
 *  penalties. Larger streaming buffers reduce refill frequency and
 *  amortize the seek cost. Pre-fetching the loop near the end of
 *  the intro is built in.
 *
 *  ---------------------------------------------------------------
 *  Typical usage
 *  ---------------------------------------------------------------
 *
 *  At startup:
 *    1. Open files / set up source for intro and loop.
 *    2. Allocate (or statically declare) the streaming buffer and
 *       two pinned head buffers.
 *    3. Configure MusicPlayerConfig and call music_create*.
 *    4. Call music_prime_intro and music_prime_loop — this calls
 *       the stream callback to fill the pinned heads. After this,
 *       start-from-stopped is instantaneous.
 *
 *  At play time:
 *    5. music_play(mp).
 *    6. Periodically (from main loop or audio task) call
 *       music_update(mp). This refills the streaming buffer if
 *       needed and pumps samples to the mixer channel.
 *    7. The mixer renders normally; the music plays.
 *
 *  ---------------------------------------------------------------
 *
 *  Depends on: audio_mixer, ring_buffer (transitively), fixed_point
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "audio/audio_mixer.h"

/* ============================================================
 *  Opaque player handle
 * ============================================================ */

typedef struct MusicPlayer MusicPlayer;

/* ============================================================
 *  Constants
 * ============================================================ */

/* Use this as intro_stream_id or loop_stream_id to indicate the
 * segment is absent. Player must have at least one segment. */
#define MUSIC_STREAM_NONE  (-1)

/* ============================================================
 *  Stream callback
 * ============================================================ */

typedef size_t (*music_stream_fn)(
    void   *user_data,
    int     stream_id,
    void   *destination,
    size_t  requested_samples,
    size_t  offset_in_stream
);

/* ============================================================
 *  Player state
 * ============================================================ */

typedef enum {
    MUSIC_STOPPED,
    MUSIC_PLAYING_INTRO,
    MUSIC_PLAYING_LOOP,
    MUSIC_PAUSED,
} MusicState;

/* ============================================================
 *  Player configuration
 *
 *  All fields must be set before calling music_create. Pointers
 *  must remain valid for the lifetime of the player. Sizes are in
 *  samples (mono) or frames (stereo) depending on the format.
 * ============================================================ */

typedef struct {
    /* Where the player feeds samples to. */
    AudioMixer       *mixer;
    size_t            mixer_channel;
    MixerSourceFormat format;        /* must match channel's format */

    /* The stream callback. user_data is opaque to the player. */
    music_stream_fn   stream_fn;
    void             *stream_user_data;

    /* Stream IDs the player passes back to the callback so it
     * knows which source to read from. Use MUSIC_STREAM_NONE for
     * absent segments. At least one of intro/loop must be set. */
    int               intro_stream_id;
    int               loop_stream_id;

    /* Streaming buffer storage — the player's working buffer for
     * pulled-from-source samples before they reach the mixer. */
    void             *streaming_buffer;
    size_t            streaming_buffer_samples;

    /* Upper bound on how many source frames the player keeps queued in
     * the mixer channel ahead of the render position. 0 = fill to the
     * channel's full capacity (the default — best underrun immunity for
     * background music / SFX, where output latency is irrelevant). A
     * small non-zero value caps the channel-buffering LATENCY, which
     * matters when the audio must stay phase-aligned with an external
     * clock (e.g. FMV A/V sync): the upstream source ring is the real
     * underrun cushion, so the channel need only hold a few render
     * quanta. Clamped to the channel capacity. */
    size_t            max_channel_fill_samples;

    /* Pinned head storage. NULL or zero size means "no pinned head
     * for this segment" — restart will incur full callback latency. */
    void             *intro_head_buffer;
    size_t            intro_head_samples;

    void             *loop_head_buffer;
    size_t            loop_head_samples;
} MusicPlayerConfig;

/* ============================================================
 *  Allocator function pointers (same types as audio_mixer)
 * ============================================================ */

/* Reuses mixer_alloc_fn / mixer_free_fn from audio_mixer.h */

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Create a music player. Returns NULL on invalid configuration or
 * allocation failure. Uses malloc/free for the player struct.
 * Caller's buffers (streaming_buffer, intro_head_buffer,
 * loop_head_buffer) are NOT allocated by the player — they're
 * referenced from the config. */
MusicPlayer *music_create(const MusicPlayerConfig *cfg);

MusicPlayer *music_create_with_allocator(const MusicPlayerConfig *cfg,
                                          mixer_alloc_fn alloc,
                                          mixer_free_fn  free_fn);

/* Destroy the player. Stops the mixer channel and resets it.
 * Safe to call with NULL. Caller's buffers are NOT freed (the
 * player doesn't own them). */
void music_destroy(MusicPlayer *mp);

/* ============================================================
 *  Pinning the heads
 *
 *  Call once per segment, after create. Invokes the stream
 *  callback to fill the pinned head buffer with the first
 *  intro_head_samples / loop_head_samples of the respective
 *  stream. Returns 0 on success, -1 if the callback returned
 *  fewer samples than requested (the segment is shorter than
 *  the head buffer) or other error. If the segment has no
 *  pinned head buffer configured (NULL or zero size), this is
 *  a no-op and returns 0.
 *
 *  Safe to call multiple times — re-fetches from the callback. */
int music_prime_intro(MusicPlayer *mp);
int music_prime_loop(MusicPlayer *mp);

/* ============================================================
 *  Playback control
 *
 *  music_play:
 *    - From STOPPED: starts from beginning of intro (or loop if
 *      no intro). Begins by playing pinned head if available.
 *    - From PAUSED: same as music_resume (treats pause/play as
 *      synonyms for convenience).
 *    - From PLAYING_*: no-op. To restart, call stop then play.
 *
 *  music_stop:
 *    - Stops playback, resets the mixer channel, transitions to
 *      STOPPED. Position is lost.
 *
 *  music_pause:
 *    - Stops feeding the mixer and stops the mixer channel.
 *      Player position is preserved.
 *    - From STOPPED: no-op.
 *
 *  music_resume:
 *    - From PAUSED: restarts the mixer channel and resumes
 *      feeding from preserved position.
 *    - From other states: no-op.
 * ============================================================ */

void music_play(MusicPlayer *mp);
void music_stop(MusicPlayer *mp);
void music_pause(MusicPlayer *mp);
void music_resume(MusicPlayer *mp);

/* ============================================================
 *  Drive the state machine
 *
 *  Call periodically from non-RT context. Does up to three things:
 *    1. Refills the player's streaming buffer from the stream
 *       callback if it's below threshold.
 *    2. Transfers samples from the streaming buffer (or pinned
 *       head, at segment start) to the mixer channel.
 *    3. Handles segment transitions: intro→loop, loop wrap-around.
 *
 *  Typical call rate: every 10-100ms is fine if buffers are sized
 *  generously. Faster is harmless. Slower risks the mixer channel
 *  starving (audible silence).
 *
 *  Safe to call in any state — no-op when stopped or paused.
 * ============================================================ */

void music_update(MusicPlayer *mp);

/* ============================================================
 *  Introspection
 * ============================================================ */

MusicState music_state(const MusicPlayer *mp);

#endif /* MUSIC_PLAYER_H */
