/* ============================================================
 *  vm_host_audio.h — VM-side audio syscall module
 *
 *  Installs the SYS_AUDIO_* ecall handlers. Each handler is the
 *  REQUESTER side of the service channel: it translates a guest call
 *  into a REQ_AUDIO_* message, posts it to the audio service, and
 *  (for calls that return a handle) waits for the response.
 *
 *  The audio service itself (audio_service.h) runs on the provider
 *  side — a worker thread on the desktop, the M4 on the H745. This
 *  module is the bridge from the guest ABI to that channel.
 *
 *  Install once per VmSystem, after the system is created, like the
 *  other vm_host_install_* modules.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_HOST_AUDIO_H
#define VM_HOST_AUDIO_H

#include "vm/vm_system.h"
#include "vm/service_channel.h"

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    /* The channel to the audio service (requester endpoint). The
     * caller owns it and the service running on the other end. */
    ServiceChannel *channel;

    /* Shared staging buffer for loading PCM into pool objects — the
     * SAME buffer handed to the audio service's config. Audio loads
     * copy guest PCM here, then post REQ_AUDIO_LOAD_STAGED. */
    void           *staging_buffer;
    size_t          staging_capacity;

    /* Timeout (ms) for synchronous channel round-trips (load/trigger
     * that return a handle). 0 -> a sensible default. */
    uint32_t        call_timeout_ms;

    /* Host filesystem root that the guest's "/host" maps to (e.g.
     * "host_files"). Used by SYS_AUDIO_LOAD_WAV to resolve a guest
     * "/host/foo.wav" path to a real host file the host reads +
     * parses. If NULL, LOAD_WAV is unavailable (returns 0). */
    const char     *host_fs_root;
} VmHostAudioConfig;

/* Register the SYS_AUDIO_* handlers on sys->ecall_router. Returns
 * false on bad args or if any registration fails (rolls back). */
bool vm_host_install_audio(VmSystem *sys, const VmHostAudioConfig *cfg);

/* Reclaim everything a dying VM owns on the audio service: its arbiter
 * tracks/voices, its pool objects, and its FFT hold. Post this from the
 * host's VM-teardown path when a guest exits or crashes, BEFORE its id
 * can be reused — otherwise audio resources (and the FFT refcount) leak.
 * Safe no-op if audio was never installed. The sweep runs on the service
 * worker thread, so it's serialized against the mixer pump. */
void vm_host_audio_sweep_vm(uint16_t vm_id);

/* #73: host-driven FMV clip audio — open/close a music_player voice fed by the
 * audio service's fmv_ring (the FMV video producer pushes the clip's audio into
 * it). open: returns a voice handle (0 = fail/none); close: stops it. Called by
 * the FMV player (open at video kickoff so play lines up with the first frame). */
uint32_t vm_host_audio_fmv_open(void);
void     vm_host_audio_fmv_close(uint32_t voice);

/* Host-driven one-shot SFX (e.g. the FMV player's bullet sound on left-click),
 * loaded into the SAME 4 MB audio pool and mixed on a free channel — layering
 * over the FMV music voice exactly like the audio_mixer demo's guest SFX.
 * _load resolves "<name>" under the /host root, parses the WAV, stages it into
 * the pool (owner vm 0); returns an object handle (0 = fail) — load once and
 * cache. _trigger plays it one-shot on a free mixer track (gain Q15, pan Q15
 * signed, 0 = centre); returns a voice handle (0 = no free track). */
uint32_t vm_host_audio_sfx_load(const char *host_relname);
uint32_t vm_host_audio_sfx_trigger(uint32_t obj, uint32_t gain_q15, int32_t pan_q15);

#endif /* VM_HOST_AUDIO_H */
