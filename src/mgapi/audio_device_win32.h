/* ============================================================
 *  audio_device_win32.h — query default audio device sample rate.
 *
 *  Internal to mgapi. Defined only on Windows; on other platforms
 *  callers should not include this header — they pick the rate via
 *  platform knowledge (e.g. MCU firmware knows its I2S clock).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_AUDIO_DEVICE_WIN32_H
#define MGAPI_AUDIO_DEVICE_WIN32_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asks WASAPI for the default render endpoint's mix-format sample
 * rate (the rate Audio Engine actually plays the device at, after
 * shared-mode mixing). Returns true on success with *out_rate_hz
 * populated; false on any failure (caller should fall back to a
 * compile-time default like 44100). */
bool mgapi_query_default_audio_rate(uint32_t *out_rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_AUDIO_DEVICE_WIN32_H */
