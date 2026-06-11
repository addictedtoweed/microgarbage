/* ============================================================
 *  audio_sink_waveout.c — live Windows audio output (winmm/waveOut)
 *
 *  A live output backend behind the audio_sink seam, for native
 *  Windows. Uses ONLY the winmm waveOut API — winmm ships with
 *  Windows and mingw provides <windows.h>/<mmsystem.h>, so there is
 *  NO extra library to install (link -lwinmm). No PortAudio, no SDL,
 *  no WASAPI/COM ceremony — waveOut is the simplest real output path
 *  and is more than adequate for a dev/test tool.
 *
 *  ---------------------------------------------------------------
 *  HONESTY: this file is the one piece of the audio stack that ships
 *  UNVERIFIED BY EXECUTION. It was written from the Win32 API
 *  contract, not tested against a real sound device (the build/test
 *  sandbox has no audio and can't run a Windows binary). The
 *  structure and the API call sequence are believed correct and each
 *  call is commented with what it expects, but whether it actually
 *  produces sound is verified on real Windows by you. If it misbehaves,
 *  the WAV-dump backend (audio_sink_wav.c) is the fallback that proves
 *  the ENGINE is correct independent of this glue.
 *  ---------------------------------------------------------------
 *
 *  Clocking: waveOut is pull-paced by the device. We keep a small
 *  pool of buffers; write() blocks until a buffer is free (the device
 *  has finished playing it), copies the caller's frames into it, and
 *  re-queues it. So the device's consumption rate paces the caller's
 *  render loop — write() blocking IS the audio clock. This keeps the
 *  push-style audio_sink seam (open/write/close) intact: the same
 *  stress harness drives WAV or waveOut unchanged.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_sink.h"

#if defined(_WIN32)

#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

/* Number of queued buffers and frames each. Several small buffers
 * keep latency modest while giving the device a queue to chew on so
 * we don't underrun between write() calls. ~4 x 1024 frames at 44.1k
 * ~= 93 ms of buffering — fine for a test tool. */
#define WO_NBUF    4
#define WO_FRAMES  1024
#define WO_BYTES   (WO_FRAMES * AUDIO_SINK_CHANNELS * (AUDIO_SINK_BITS / 8))

typedef struct {
    HWAVEOUT   h;
    WAVEHDR    hdr[WO_NBUF];
    uint8_t   *buf[WO_NBUF];
    int        next;          /* round-robin buffer to reuse next  */
    int        prepared[WO_NBUF];
    HANDLE     evt;           /* signaled by the callback on done   */
} WoCtx;

/* waveOut calls this back (on its own thread) when a buffer finishes.
 * We just signal the event so a blocked write() can wake and reclaim
 * a buffer. Keep this minimal — no heavy work in the callback. */
static void CALLBACK wo_callback(HWAVEOUT h, UINT msg, DWORD_PTR inst,
                                 DWORD_PTR p1, DWORD_PTR p2) {
    (void)h; (void)p1; (void)p2;
    if (msg == WOM_DONE) {
        WoCtx *c = (WoCtx *)inst;
        SetEvent(c->evt);     /* wake any waiter */
    }
}

static void *waveout_open(uint32_t sample_rate) {
    WoCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->evt = CreateEvent(NULL, FALSE, FALSE, NULL);  /* auto-reset */
    if (!c->evt) { free(c); return NULL; }

    /* Describe the stream: PCM, stereo, 16-bit, at sample_rate. */
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = AUDIO_SINK_CHANNELS;
    wf.nSamplesPerSec  = sample_rate;
    wf.wBitsPerSample  = AUDIO_SINK_BITS;
    wf.nBlockAlign     = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;

    /* Open the default output device. CALLBACK_FUNCTION routes
     * WOM_DONE to wo_callback; dwInstance carries our ctx. */
    MMRESULT r = waveOutOpen(&c->h, WAVE_MAPPER, &wf,
                             (DWORD_PTR)wo_callback, (DWORD_PTR)c,
                             CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        CloseHandle(c->evt);
        free(c);
        return NULL;
    }

    /* Allocate + prepare the buffer pool. Each WAVEHDR points at its
     * own byte buffer; we mark them all "done" initially (lpData set,
     * dwFlags WHDR_DONE) so write() sees them as free. */
    for (int i = 0; i < WO_NBUF; i++) {
        c->buf[i] = malloc(WO_BYTES);
        if (!c->buf[i]) { /* partial cleanup */
            for (int j = 0; j < i; j++) free(c->buf[j]);
            waveOutClose(c->h); CloseHandle(c->evt); free(c);
            return NULL;
        }
        memset(&c->hdr[i], 0, sizeof(WAVEHDR));
        c->hdr[i].lpData         = (LPSTR)c->buf[i];
        c->hdr[i].dwBufferLength = WO_BYTES;
        c->hdr[i].dwFlags        = WHDR_DONE;  /* free to fill */
        c->prepared[i]           = 0;
    }
    c->next = 0;
    return c;
}

/* Block until buffer `i` is free (the device finished it or it was
 * never queued). WHDR_DONE is set by the driver when playback of that
 * buffer completes. */
static void wait_for_buffer(WoCtx *c, int i) {
    while (!(c->hdr[i].dwFlags & WHDR_DONE)) {
        /* sleep until the callback signals a completion, then re-check.
         * (auto-reset event; a spurious wake just re-tests the flag.) */
        WaitForSingleObject(c->evt, 50);
    }
}

static int waveout_write(void *ctx, const int16_t *interleaved, uint32_t frames) {
    WoCtx *c = (WoCtx *)ctx;
    if (!c) return -1;

    uint32_t done = 0;
    while (done < frames) {
        int i = c->next;
        wait_for_buffer(c, i);                 /* blocks: this is the clock */

        /* If this buffer was previously queued+prepared, unprepare it
         * before reusing (required by the API once WHDR_DONE). */
        if (c->prepared[i]) {
            waveOutUnprepareHeader(c->h, &c->hdr[i], sizeof(WAVEHDR));
            c->prepared[i] = 0;
        }

        /* Fill up to one buffer's worth of frames. */
        uint32_t chunk = frames - done;
        if (chunk > WO_FRAMES) chunk = WO_FRAMES;
        uint32_t bytes = chunk * AUDIO_SINK_CHANNELS * (AUDIO_SINK_BITS / 8);
        memcpy(c->buf[i], interleaved + (size_t)done * AUDIO_SINK_CHANNELS, bytes);

        c->hdr[i].dwBufferLength = bytes;
        c->hdr[i].dwFlags        = 0;          /* clear WHDR_DONE       */

        if (waveOutPrepareHeader(c->h, &c->hdr[i], sizeof(WAVEHDR))
                != MMSYSERR_NOERROR) return -1;
        c->prepared[i] = 1;
        if (waveOutWrite(c->h, &c->hdr[i], sizeof(WAVEHDR))
                != MMSYSERR_NOERROR) return -1;

        c->next = (i + 1) % WO_NBUF;
        done += chunk;
    }
    return (int)done;
}

static void waveout_close(void *ctx) {
    WoCtx *c = (WoCtx *)ctx;
    if (!c) return;

    /* Wait for all queued buffers to finish so we don't cut off audio,
     * then unprepare + free. */
    for (int i = 0; i < WO_NBUF; i++) wait_for_buffer(c, i);
    waveOutReset(c->h);                        /* stop anything pending */
    for (int i = 0; i < WO_NBUF; i++) {
        if (c->prepared[i])
            waveOutUnprepareHeader(c->h, &c->hdr[i], sizeof(WAVEHDR));
        free(c->buf[i]);
    }
    waveOutClose(c->h);
    CloseHandle(c->evt);
    free(c);
}

const AudioSinkBackend audio_sink_waveout = {
    "waveout", waveout_open, waveout_write, waveout_close
};

#endif /* _WIN32 */
