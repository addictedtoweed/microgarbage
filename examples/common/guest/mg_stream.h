/* ============================================================
 *  mg_stream.h — guest-side wrapper for the host stream arbiter
 *
 *  Lets a guest VM register a (path, chunk_bytes, depth) stream
 *  with the host's round-robin SD/host-file arbiter, then pull
 *  one fixed-size chunk at a time. The arbiter pre-reads ahead
 *  of the consumer; consume() typically returns instantly with a
 *  ring-staged chunk.
 *
 *  USAGE (FMV demo style):
 *      MgStream s = mg_stream_open(
 *          "/host/movie.fmv",
 *          AUDIO_BYTES + VIDEO_BYTES,   // one A/V frame per chunk
 *          4);                          // 4 chunks of look-ahead
 *      uint8_t buf[AUDIO_BYTES + VIDEO_BYTES];
 *      while (mg_stream_consume(s, buf, sizeof buf)) {
 *          // process one full frame from buf
 *      }
 *      mg_stream_close(s);
 *
 *  The consume call blocks (via short sleep_ticks retries)
 *  until a chunk is available or true EOF is reached. EOF
 *  returns false; ring-empty waits.
 *
 *  Note: the guest still owns the underlying fd if it opens via
 *  the alt entry mg_stream_open_fd. The default mg_stream_open
 *  bundles open+register and close+unregister symmetrically so
 *  the caller doesn't have to track an extra fd.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_STREAM_H
#define MG_STREAM_H

#include <stdbool.h>
#include <stdint.h>

/* Opaque handle. Negative = invalid. */
typedef int MgStream;
#define MG_STREAM_INVALID (-1)

/* Open + register. Returns a handle ≥ 0 or MG_STREAM_INVALID.
 *   path        : forwarded to fs_open(path, O_RDONLY).
 *   chunk_bytes : fixed read amount per arbiter chunk. Must be
 *                 > 0 and ≤ 64 KB.
 *   depth       : ring slot count, power of two ≥ 2 (usable
 *                 depth = depth - 1). 4 is a reasonable default
 *                 — that's 3 chunks of look-ahead.
 * On failure the underlying fd (if opened) is closed before
 * returning. */
MgStream mg_stream_open(const char *path,
                        uint32_t chunk_bytes,
                        uint32_t depth);

/* Variant that registers against a fd the guest already has open.
 * Useful when the caller wants to read a small header first then
 * stream the rest. The arbiter does NOT take ownership of the fd
 * — caller still closes it after mg_stream_close. */
MgStream mg_stream_open_fd(int fd,
                           uint32_t chunk_bytes,
                           uint32_t depth);

/* Block until one chunk_bytes-sized chunk has been copied into
 * dst, or until the stream drains past EOF. Returns:
 *   true  : full chunk copied; dst is valid for chunk_bytes.
 *   false : EOF reached and ring drained — no more data ever.
 *
 * dst_cap must be ≥ chunk_bytes passed at open. The blocking
 * impl uses sys_sleep_ticks(SLEEP_MS) between retries; while
 * sleeping the host arbiter ticks fill the ring. */
bool mg_stream_consume(MgStream s, void *dst, uint32_t dst_cap);

/* Non-blocking peek: true if EOF reached AND ring drained. */
bool mg_stream_eof(MgStream s);

/* Close + (if mg_stream_open opened it) close the underlying fd.
 * Idempotent for an invalid handle. */
void mg_stream_close(MgStream s);

#endif /* MG_STREAM_H */
