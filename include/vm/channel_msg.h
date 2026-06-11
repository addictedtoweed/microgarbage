/* ============================================================
 *  channel_msg.h — inter-core service channel message format
 *
 *  The fixed-size message that travels over the service channel
 *  (docs/intercore-channel.md). 32 bytes, 8 x uint32, so a whole
 *  message is one cache-friendly unit and the SPSC ring element
 *  size is fixed.
 *
 *  Large payloads (PCM blocks, file buffers) are NOT inlined here —
 *  they are passed by shared-buffer handle + offset + length in the
 *  arg fields, with the data living in shared memory the provider
 *  can reach. The ring only ever moves these 32-byte control
 *  messages.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef CHANNEL_MSG_H
#define CHANNEL_MSG_H

#include <stdint.h>

/* 32-byte message. Layout is fixed and asserted below so it is
 * identical on the desktop and on both H745 cores. */
typedef struct {
    uint16_t type;      /* REQ_* / RESP_* opcode (namespaced, below) */
    uint16_t flags;     /* CHANNEL_FLAG_*                            */
    uint32_t seq;       /* request sequence; response echoes it      */
    uint32_t a0;        /* arg / handle                              */
    uint32_t a1;        /* arg / size / offset                       */
    uint32_t a2;        /* arg                                       */
    uint32_t a3;        /* arg  (responses: result/status code)      */
    uint32_t a4;        /* arg  (responses: returned handle)         */
    uint32_t reserved;  /* future use; senders must zero it          */
} ChannelMsg;

_Static_assert(sizeof(ChannelMsg) == 32, "ChannelMsg must be 32 bytes");

/* ---- flags ---- */
enum {
    CHANNEL_FLAG_EXPECTS_RESPONSE = 1u << 0, /* provider must reply        */
    CHANNEL_FLAG_ASYNC            = 1u << 1, /* requester won't block-wait */
};

/* ---- opcode namespaces (high byte) ----
 * 0x00xx control/diagnostic, 0x01xx audio, 0x02xx file, 0x03xx gfx.
 * Responses use the matching request opcode; the message direction
 * (which ring it arrived on) plus the flags distinguish req vs resp,
 * and `seq` correlates them. */
enum {
    /* control */
    MSG_NOP        = 0x0000,
    MSG_ECHO       = 0x0001,   /* test/diagnostic: provider replies, a0..a4 copied */
    MSG_PING       = 0x0002,

    /* audio (0x01xx) — wired in a later step */
    REQ_AUDIO_POOL_ALLOC   = 0x0100,
    REQ_AUDIO_POOL_FREE    = 0x0101,
    REQ_AUDIO_LOAD_SAMPLE  = 0x0102,
    REQ_AUDIO_LOAD_MUSIC   = 0x0103,
    REQ_AUDIO_TRIGGER_SFX  = 0x0104,
    REQ_AUDIO_PLAY_MUSIC   = 0x0105,
    REQ_AUDIO_VOICE_STOP   = 0x0106,
    REQ_AUDIO_STOP_MUSIC   = 0x0107,
    /* Load PCM from the shared staging buffer into a new pool object.
     * a0 = byte size, a1 = owner_vm. The requester copies PCM into the
     * service's staging buffer (set at create time) before posting;
     * the service copies staging->pool (service-side, no race) and
     * returns the object handle. Size must be <= staging capacity. */
    REQ_AUDIO_LOAD_STAGED  = 0x0108,
    REQ_AUDIO_SET_GAIN     = 0x0109,  /* a0 = voice, a1 = gain q15 */
    /* FFT band meter over the final mixed output. */
    REQ_AUDIO_FFT_ENABLE   = 0x010A,  /* a0 = enable(1)/0, a1 = owner_vm  */
    /* Get band levels. The response packs up to 16 band bytes (0..255)
     * into a0..a3 (4 bytes each, little-endian); a4 = band count. */
    REQ_AUDIO_GET_LEVELS   = 0x010B,
    /* Stream a long WAV from a file rather than loading it into the
     * pool. The resolved native path sits NUL-terminated in the shared
     * staging buffer; a0 = path length (incl. NUL), a1 = owner_vm. The
     * service opens it via its AudioFileReader, builds a looping
     * file-stream music voice, and returns the voice handle. No pool
     * object is involved (the file is read incrementally, service-side).
     * This is how arbitrarily long songs play on desktop (stdio +
     * trashfs) and, on an MCU port, off SD/flash. */
    REQ_AUDIO_STREAM_WAV   = 0x010C,
    /* Reclaim everything a dying VM owns: stop its arbiter tracks/voices,
     * free its pool objects, release its FFT hold. a0 = vm_id. Posted by
     * the host VM-teardown hook so a crashed/exited guest can't leak audio
     * resources or strand its FFT refcount. Runs on the worker thread, so
     * it's serialized against the mixer pump (no locking the pool/arbiter
     * out from under the audio callback). */
    REQ_AUDIO_SWEEP_VM     = 0x010D,
    /* PCM streaming voice — guest pushes raw int16 stereo frames into
     * a mixer channel one chunk at a time.
     *
     * OPEN: a0 = sample_rate, a1 = channels (1=mono→L=R, 2=stereo),
     *       a2 = owner_vm. Returns a voice handle. The voice is alive
     *       until CLOSE (or VM teardown via SWEEP_VM).
     *
     * FEED: a0 = voice, a1 = frame_count to feed. PCM bytes are
     *       already in the shared staging buffer (the caller copies
     *       int16 stereo frames there before posting). The service
     *       pushes them into the voice's mixer channel ring.
     *       Returns frames actually fed (may be less than requested
     *       if the channel ring is full). Caller retries with the
     *       remainder.
     *
     * CLOSE: a0 = voice. Stops + frees the voice/track. */
    REQ_AUDIO_PCM_STREAM_OPEN  = 0x010E,
    REQ_AUDIO_PCM_STREAM_FEED  = 0x010F,
    REQ_AUDIO_PCM_STREAM_CLOSE = 0x0110,

    /* file (0x02xx) — reserved for the M4-owns-SD proxy */
    REQ_FILE_OPEN   = 0x0200,
    REQ_FILE_READ   = 0x0201,
    REQ_FILE_WRITE  = 0x0202,
    REQ_FILE_CLOSE  = 0x0203,
};

#endif /* CHANNEL_MSG_H */
