/* ============================================================
 *  audio_device_win32.c — query the default audio render device's
 *  preferred sample rate via WASAPI.
 *
 *  Why this exists: legacy waveOut accepts whatever rate we ask for
 *  but doesn't always invoke Audio Engine's sample-rate converter,
 *  especially on integrated chipsets / mixed-mode endpoints. Result:
 *  we ask for 44.1 kHz, waveOut nominally accepts, the device drains
 *  at its actual native rate (often 48 kHz on Win11 default; can be
 *  32 kHz on some chipsets), and audio plays at the wrong tempo.
 *
 *  Fix: ask the device what rate it wants, configure both the mixer
 *  AND the waveOut sink at that rate, let the mixer's own resampler
 *  handle 44.1-kHz source streams. Single round of resampling on the
 *  way in (cheap, where we already have the code) instead of an
 *  unreliable one on the way out (where the OS may decline to do it).
 *
 *  Win32 only. The MCU build hardcodes its rate at link time and
 *  doesn't compile this file.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#if defined(_WIN32)

/* INITGUID must precede the COM headers so the linker gets the GUID
 * data instantiated in THIS TU rather than relying on -luuid. mingw's
 * libuuid has these but defining INITGUID locally is faster than
 * checking that the import library is on every toolchain. */
#define INITGUID
#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "audio_device_win32.h"

#include <stdint.h>
#include <stdio.h>

bool mgapi_query_default_audio_rate(uint32_t *out_rate_hz) {
    if (!out_rate_hz) return false;

    HRESULT hr;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice           *device     = NULL;
    IAudioClient        *client     = NULL;
    WAVEFORMATEX        *fmt        = NULL;
    bool ok = false;

    /* COM init: tolerate already-initialized callers. RPC_E_CHANGED_MODE
     * means somebody else picked a different apartment — fine, just
     * don't pair with CoUninitialize at the end. S_FALSE means we
     * re-incremented an existing init — still need to pair. */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com_pair = SUCCEEDED(hr);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) goto done;

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,
                                                     eRender, eConsole,
                                                     &device);
    if (FAILED(hr)) goto done;

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                            NULL, (void **)&client);
    if (FAILED(hr)) goto done;

    hr = IAudioClient_GetMixFormat(client, &fmt);
    if (FAILED(hr)) goto done;

    *out_rate_hz = (uint32_t)fmt->nSamplesPerSec;
    ok = true;

done:
    if (fmt)        CoTaskMemFree(fmt);
    if (client)     IAudioClient_Release(client);
    if (device)     IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (com_pair)   CoUninitialize();
    return ok;
}

#endif  /* _WIN32 */
