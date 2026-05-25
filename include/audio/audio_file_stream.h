/* ============================================================
 *  audio_file_stream.h — a music_player stream source backed by a file
 *
 *  Streams arbitrarily long PCM from a WAV file into a music voice,
 *  reading incrementally instead of loading the whole file into the
 *  audio pool. It implements the music_player `music_stream_fn`
 *  contract, so it drops into the same machinery as the pool-backed
 *  source (audio_pool_stream).
 *
 *  Platform seam: all file I/O goes through an AudioFileReader vtable.
 *  The desktop supplies a stdio reader (the /host directory) and a
 *  trashfs reader (the /td0 volume); an MCU port supplies its own
 *  SD/flash reader. The same streaming source code runs on both —
 *  only the reader differs.
 *
 *  Output is INTERLEAVED STEREO PCM16 (the mixer's music format):
 *  stereo WAVs are preserved as-is, mono is promoted to L==R (no
 *  downmix), 8-bit promoted to 16-bit. 8/16-bit, mono/stereo WAV
 *  accepted. Source-rate != output-rate resampling is not handled here
 *  yet (the wav should be at the output rate); that's a later
 *  refinement on the mixer channel.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_FILE_STREAM_H
#define AUDIO_FILE_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Platform file-reader seam. Offsets and counts are BYTES. open()
 * returns an opaque handle (NULL on failure); the other ops take it
 * back. The service stays filesystem-agnostic — the platform binds
 * stdio / trashfs / SD here. */
typedef struct {
    void    *(*open)(void *ctx, const char *path);
    uint32_t (*read)(void *ctx, void *fh, void *dst, uint32_t bytes);
    bool     (*seek)(void *ctx, void *fh, uint32_t byte_off);
    void     (*close)(void *ctx, void *fh);
    void     *ctx;
} AudioFileReader;

/* Source bytes pulled per read chunk. Small so the MCU footprint is
 * modest; reads loop to satisfy larger requests. */
#ifndef AUDIO_FILE_STREAM_SCRATCH
#define AUDIO_FILE_STREAM_SCRATCH  4096u
#endif

typedef struct {
    AudioFileReader reader;          /* copied in at open */
    void    *fh;                     /* open file handle, NULL when closed */
    uint32_t data_offset;            /* byte offset of PCM data in the file */
    uint32_t data_bytes;             /* total PCM data bytes (true size) */
    uint16_t channels;               /* 1 or 2 */
    uint16_t bits;                   /* 8 or 16 */
    uint32_t sample_rate;            /* informational (resampling is TODO) */
    uint32_t src_frame_bytes;        /* channels * bits/8 */
    uint8_t  scratch[AUDIO_FILE_STREAM_SCRATCH];
} AudioFileStreamCtx;

/* Open `path` via `reader` and parse the WAV header so the source is
 * ready to stream. Returns false (leaving nothing open) on open failure,
 * a parse error, or an unsupported format. */
bool audio_file_stream_open(AudioFileStreamCtx *ctx,
                            const AudioFileReader *reader, const char *path);

/* music_stream_fn implementation: deliver up to `requested_samples`
 * FRAMES of interleaved stereo16 (2 int16 each) starting at
 * `offset_in_stream` (frames). `destination` must hold
 * requested_samples*2 int16. Returns frames produced; fewer than
 * requested signals end-of-stream (the player then loops / transitions).
 * `stream_id` is accepted for signature compatibility — a file source
 * serves one stream. */
size_t audio_file_stream_read(void *user_data, int stream_id,
                              void *destination,
                              size_t requested_samples,
                              size_t offset_in_stream);

/* Close the underlying file. Safe to call more than once. */
void audio_file_stream_close(AudioFileStreamCtx *ctx);

#endif /* AUDIO_FILE_STREAM_H */
