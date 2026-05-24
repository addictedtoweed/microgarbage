/* ============================================================
 *  audio_pool_stream.h — bridge the block pool to the music player
 *
 *  The music player (music_player.h) pulls source audio through a
 *  music_stream_fn callback that works in SAMPLES (frames) and is
 *  given an opaque stream_id telling it which source to read. The
 *  audio pool (audio_pool.h) stores object data addressed in BYTES
 *  by handle.
 *
 *  This adapter is the glue: it provides a music_stream_fn that maps
 *  a stream_id to a pool object handle, converts the player's frame-
 *  based requests to byte-based pool reads (using the format's bytes-
 *  per-frame), and returns the number of frames delivered. So "play
 *  music from a sound-RAM handle" = configure the player with this
 *  callback and a context binding intro/loop handles.
 *
 *  Non-contiguity is invisible here: audio_pool_read walks the block
 *  chain, so the player sees a flat sample stream. The player then
 *  copies into its (contiguous) streaming buffer, which the mixer
 *  renders into the (contiguous) output — exactly the data flow in
 *  docs/audio-architecture.md.
 *
 *  This adapter is the SD-stream's equivalent for resident samples:
 *  an SD source would read from a file; this reads from PSRAM blocks.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_POOL_STREAM_H
#define AUDIO_POOL_STREAM_H

#include "audio/audio_pool.h"
#include "audio/music_player.h"
#include "audio/audio_mixer.h"

#include <stddef.h>

/* Up to two stream IDs per player (intro + loop). The context maps
 * each configured stream_id to a pool object handle. */
typedef struct {
    AudioPool        *pool;
    /* binding: stream_id -> object handle. Two slots; either may be
     * unused (handle == AUDIO_POOL_HANDLE_NONE). */
    int               stream_id[2];
    AudioObjHandle    handle[2];
    size_t            bytes_per_frame;   /* of the SOURCE (pool) data  */
    /* When set, the pool holds mono16 but the player/channel is
     * stereo16: each mono frame is promoted in place to L==R after the
     * read (no downmix — a lossless duplication). bytes_per_frame stays
     * the source's (2); `destination` must hold 2x the int16. */
    bool              promote_stereo;
} AudioPoolStreamCtx;

/* Initialize a context. `format` sets bytes-per-frame (must match the
 * player's channel format). Both bindings start empty; set them with
 * audio_pool_stream_bind. Returns false on bad args. */
bool audio_pool_stream_init(AudioPoolStreamCtx *ctx, AudioPool *pool,
                            MixerSourceFormat format);

/* Enable mono16-source -> stereo16-output promotion (L==R). Use when
 * the pool data is mono16 but the player/channel format is stereo16
 * (the "full stereo mixer" path). Off by default. */
void audio_pool_stream_set_promote_stereo(AudioPoolStreamCtx *ctx, bool on);

/* Bind a stream_id to a pool object handle (e.g. the player's
 * intro_stream_id -> the intro sample's handle). Up to two bindings.
 * Returns false if both slots are already used by other ids. Passing
 * AUDIO_POOL_HANDLE_NONE clears a binding for that id. */
bool audio_pool_stream_bind(AudioPoolStreamCtx *ctx,
                            int stream_id, AudioObjHandle handle);

/* The music_stream_fn to hand to the player's config. Set
 * cfg.stream_fn = audio_pool_stream_read and
 * cfg.stream_user_data = (your AudioPoolStreamCtx*). */
size_t audio_pool_stream_read(void *user_data, int stream_id,
                              void *destination,
                              size_t requested_samples,
                              size_t offset_in_stream);

#endif /* AUDIO_POOL_STREAM_H */
