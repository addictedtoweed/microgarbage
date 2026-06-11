/* ============================================================
 *  audio_sink_wasapi.c — Windows shared-mode WASAPI sink
 *
 *  Why this exists: the waveOut sink (audio_sink_waveout.c) ships
 *  through a legacy WDM path on Win11 that, on some chipsets, does
 *  NOT correctly honor the rate it was configured at — sink_write
 *  blocks longer than the nominal frame-count-divided-by-rate
 *  would suggest, and audio drains at ~76% of the requested rate.
 *  v1.93–v1.96 chased that 76% figure across every other moving
 *  piece (timer granularity, Sleep cycles, partial quanta, source/
 *  output rate mismatch); the constant 76% across every sink rate
 *  proved waveOut itself was the bottleneck.
 *
 *  WASAPI in shared mode goes through the modern Windows Audio
 *  Engine path — Initialize() at the device's mix-format rate (or
 *  a converted format with AUTOCONVERTPCM), event-driven Write that
 *  blocks on the engine's per-period event. The rate is honest and
 *  the OS owns sample-rate conversion if our format differs from
 *  the engine's mix format.
 *
 *  Architecture:
 *    - shared-mode, event-driven.
 *    - 100 ms buffer (REFERENCE_TIME = 1,000,000 hundredths-of-ns).
 *    - WAVEFORMATEX PCM 16-bit stereo at the requested rate.
 *    - AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM so the engine resamples
 *      and reformats if our 16-bit-PCM-at-N-Hz doesn't match the
 *      engine's mix format directly.
 *    - SetEventHandle gives us an auto-signaled HANDLE that fires
 *      each time the engine consumes a period of audio; the write
 *      loop blocks on this event for pacing.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#if defined(_WIN32)

/* COM machinery; INITGUID instantiates the GUIDs in this TU instead
 * of relying on -luuid. COBJMACROS gives us the C-style IFoo_Method
 * helpers so we don't have to type c->client->lpVtbl->Method(...). */
#define INITGUID
#define COBJMACROS

#include "audio/audio_sink.h"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

typedef struct {
    IAudioClient       *client;
    IAudioRenderClient *render;
    HANDLE              evt;
    UINT32              buffer_frames;
    UINT32              channels;
    bool                started;
    bool                com_pair;     /* CoUninitialize on close */
} WasapiCtx;

static void *wasapi_open(uint32_t sample_rate) {
    HRESULT hr;
    WasapiCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    /* COM init — tolerate already-initialized callers. */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    c->com_pair = SUCCEEDED(hr);

    IMMDeviceEnumerator *enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) goto fail;

    IMMDevice *device = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,
                                                     eRender, eConsole,
                                                     &device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr)) goto fail;

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                            NULL, (void **)&c->client);
    IMMDevice_Release(device);
    if (FAILED(hr)) goto fail;

    /* Format: PCM int16 stereo at caller's rate. AUTOCONVERTPCM lets
     * the engine resample if it doesn't match the mix format. */
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = AUDIO_SINK_CHANNELS;
    wf.nSamplesPerSec  = sample_rate;
    wf.wBitsPerSample  = AUDIO_SINK_BITS;
    wf.nBlockAlign     = (WORD)(wf.nChannels * (wf.wBitsPerSample / 8));
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;
    c->channels = wf.nChannels;

    /* 100 ms buffer in 100-ns units. Shared mode with event callback +
     * the auto-PCM-converter flags so any rate / bit-depth mismatch
     * with the mix format is handled by Audio Engine SRC instead of
     * failing the Initialize call. */
    REFERENCE_TIME buffer_duration = (REFERENCE_TIME)100 * 10000;
    hr = IAudioClient_Initialize(c->client,
                                  AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  buffer_duration,
                                  0,
                                  &wf, NULL);
    if (FAILED(hr)) goto fail;

    c->evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!c->evt) goto fail;
    hr = IAudioClient_SetEventHandle(c->client, c->evt);
    if (FAILED(hr)) goto fail;

    hr = IAudioClient_GetBufferSize(c->client, &c->buffer_frames);
    if (FAILED(hr)) goto fail;

    hr = IAudioClient_GetService(c->client, &IID_IAudioRenderClient,
                                  (void **)&c->render);
    if (FAILED(hr)) goto fail;

    /* Pre-fill the first buffer with silence so the engine has
     * something to play immediately — avoids a startup click and
     * keeps the event signaling steady from the first iteration. */
    BYTE *data = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(c->render, c->buffer_frames,
                                                &data)) && data) {
        memset(data, 0,
               (size_t)c->buffer_frames * c->channels * (AUDIO_SINK_BITS / 8));
        IAudioRenderClient_ReleaseBuffer(c->render, c->buffer_frames,
                                          AUDCLNT_BUFFERFLAGS_SILENT);
    }

    hr = IAudioClient_Start(c->client);
    if (FAILED(hr)) goto fail;
    c->started = true;
    return c;

fail:
    if (c->render) IAudioRenderClient_Release(c->render);
    if (c->client) IAudioClient_Release(c->client);
    if (c->evt)    CloseHandle(c->evt);
    if (c->com_pair) CoUninitialize();
    free(c);
    return NULL;
}

static int wasapi_write(void *ctx, const int16_t *interleaved,
                        uint32_t frames) {
    WasapiCtx *c = (WasapiCtx *)ctx;
    if (!c) return -1;

    uint32_t done = 0;
    while (done < frames) {
        /* Wait for engine to consume a period; 200 ms safety timeout
         * just in case the device goes idle. */
        DWORD wr = WaitForSingleObject(c->evt, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) return -1;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(c->client, &padding)))
            return -1;
        UINT32 available = c->buffer_frames - padding;
        if (available == 0) continue;

        UINT32 chunk = frames - done;
        if (chunk > available) chunk = available;

        BYTE *data = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(c->render, chunk, &data))
            || !data) return -1;

        const size_t bytes_per_frame =
            (size_t)c->channels * (AUDIO_SINK_BITS / 8);
        memcpy(data,
               interleaved + (size_t)done * c->channels,
               (size_t)chunk * bytes_per_frame);
        IAudioRenderClient_ReleaseBuffer(c->render, chunk, 0);
        done += chunk;
    }
    return (int)done;
}

static void wasapi_close(void *ctx) {
    WasapiCtx *c = (WasapiCtx *)ctx;
    if (!c) return;
    if (c->started) IAudioClient_Stop(c->client);
    if (c->render)  IAudioRenderClient_Release(c->render);
    if (c->client)  IAudioClient_Release(c->client);
    if (c->evt)     CloseHandle(c->evt);
    if (c->com_pair) CoUninitialize();
    free(c);
}

const AudioSinkBackend audio_sink_wasapi = {
    "wasapi", wasapi_open, wasapi_write, wasapi_close
};

#endif /* _WIN32 */
