/* ============================================================
 *  host_audio.h — audio service bring-up for the shell host.
 *
 *  Stands up the AudioService on its own worker thread (the
 *  desktop stand-in for the H745's M4 core), opens the waveOut
 *  sink on Windows, and installs the SYS_AUDIO_* ecall handlers
 *  on the supplied VmSystem so guests can request playback.
 *
 *  Storage is static (audio pool + staging + channel rings live
 *  inside this module — there is exactly one host audio service
 *  per process), so init takes only the things the file reader
 *  can't look up itself: the VmSystem to install handlers on,
 *  the /host filesystem root for "/host/x.wav" lookups, and the
 *  trashfs volume for "td0:/x.wav" lookups.
 *
 *  HOST_AUDIO_SUPPORTED stays defined on every desktop target
 *  (the live waveOut sink is Windows-only, but the silent loop
 *  works everywhere). Callers gate the call site on it for
 *  symmetry with the PTY/TCP modules.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_AUDIO_H
#define HOST_AUDIO_H

#include <stdbool.h>

/* VmSystem and TrashfsVolume are tag-less typedefs — we can't
 * forward-declare them, so the headers come along. They're cheap
 * and host.c already pulls them both in directly. */
#include "vm/vm_system.h"
#include "storage/trashfs.h"

#define HOST_AUDIO_SUPPORTED 1

/* Start the audio worker + install audio ecalls. Returns true on
 * success. On failure the audio side is fully torn down (caller
 * may proceed without audio — the shell still runs). */
bool host_audio_start(VmSystem *sys,
                      const char *host_fs_root,
                      TrashfsVolume *trash_vol);

/* Idempotent stop. Safe to call on a failed-start or never-started
 * audio system. */
void host_audio_stop(void);

#endif /* HOST_AUDIO_H */
