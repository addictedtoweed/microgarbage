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
} VmHostAudioConfig;

/* Register the SYS_AUDIO_* handlers on sys->ecall_router. Returns
 * false on bad args or if any registration fails (rolls back). */
bool vm_host_install_audio(VmSystem *sys, const VmHostAudioConfig *cfg);

#endif /* VM_HOST_AUDIO_H */
