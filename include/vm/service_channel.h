/* ============================================================
 *  service_channel.h — full-duplex inter-core service channel
 *
 *  Two SPSC rings forming a request/response pair between a
 *  REQUESTER (the VM side: M7 / desktop main thread) and a PROVIDER
 *  (the service side: M4 / desktop worker thread):
 *
 *      requester --post--> [ request ring ] --poll--> provider
 *      requester <--poll-- [ response ring ] <--post-- provider
 *
 *  Each ring is single-producer/single-consumer (spsc_ring), so as
 *  long as exactly one context acts as requester and one as
 *  provider, the whole thing is lock-free — correctness rests on the
 *  rings' acquire/release ordering, not on any mutex.
 *
 *  The ring MEMORY and message movement are transport-independent
 *  (just shared memory + the SPSC protocol). What differs per
 *  platform is only how a sleeping peer is WOKEN and how a waiter
 *  BLOCKS — that is the ChannelTransport vtable:
 *
 *      desktop:  notify = condvar signal,  wait = condvar wait
 *      H745:     notify = HSEM/IPI,         wait = WFE on HSEM
 *
 *  So the channel == rings (portable) + transport (per platform).
 *  Service code and VM-side code talk only to this API; only the
 *  transport is swapped at port time. (Third instance of the
 *  "swap the backend per platform" pattern, after VmHostTransport
 *  and the host_platform HAL.)
 *
 *  ---------------------------------------------------------------
 *  Endpoint discipline (preserves the SPSC guarantee)
 *  ---------------------------------------------------------------
 *  Use the requester-side calls from the requester context ONLY and
 *  the provider-side calls from the provider context ONLY:
 *
 *    requester context:  channel_request_post / channel_response_poll
 *    provider context:   channel_request_poll / channel_response_post
 *
 *  Each ring then has exactly one producer and one consumer, which
 *  is what spsc_ring requires. (On the cooperative VM scheduler,
 *  multiple VMs share the single requester context but never run
 *  concurrently, so "one producer" still holds — see the invariant
 *  in docs/audio-architecture.md.)
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef SERVICE_CHANNEL_H
#define SERVICE_CHANNEL_H

#include "vm/channel_msg.h"
#include "containers/spsc_ring.h"

#include <stdint.h>
#include <stdbool.h>

/* ---- transport vtable: only wake/block differ per platform ---- */
typedef struct {
    /* Wake a peer that may be blocked in `wait`. May be a no-op if
     * the peer only polls. Called after a successful post. */
    void (*notify)(void *ctx);

    /* Block the caller until notified or timeout_ms elapses (0 =
     * return immediately, UINT32_MAX = wait forever). Returns true
     * if woken by a notify, false on timeout. May be a cooperative
     * spin on platforms without a blocking primitive. */
    bool (*wait)(void *ctx, uint32_t timeout_ms);

    /* Destroy transport-private state (condvar/mutex). The channel's
     * own memory is caller-owned and not freed here. */
    void (*destroy)(void *ctx);

    void *ctx;   /* transport-private (condvar+mutex, HSEM id, ...) */
} ChannelTransport;

/* ---- the channel ---- */
typedef struct {
    SpscRing req;    /* requester produces, provider consumes */
    SpscRing resp;   /* provider produces, requester consumes */
    ChannelTransport transport;
    _Atomic uint32_t next_seq;   /* requester-side seq allocator */
} ServiceChannel;

/* Initialize a channel over caller-provided ring storage. Each ring
 * needs `slots` slots of sizeof(ChannelMsg) bytes; `slots` must be a
 * power of two >= 2 (usable depth slots-1). `req_storage` and
 * `resp_storage` must each be at least slots*sizeof(ChannelMsg)
 * bytes and outlive the channel. `transport` is copied in (its ctx
 * pointer is retained). Returns false on bad args. NOT concurrency-
 * safe — call during setup. */
bool service_channel_init(ServiceChannel *ch,
                          void *req_storage, void *resp_storage,
                          size_t slots,
                          const ChannelTransport *transport);

/* Tear down: invokes transport.destroy. NOT concurrency-safe. */
void service_channel_destroy(ServiceChannel *ch);

/* Allocate a fresh sequence number (requester side). Monotonic,
 * wraps. Use it to stamp a request and match its response. */
uint32_t service_channel_next_seq(ServiceChannel *ch);

/* ---- requester-side (VM context) ---- */

/* Post a request. Returns true on success, false if the request
 * ring is full (caller backs off / retries). Notifies the provider
 * on success. */
bool channel_request_post(ServiceChannel *ch, const ChannelMsg *msg);

/* Try to pop one response. Returns true if one was available. */
bool channel_response_poll(ServiceChannel *ch, ChannelMsg *out);

/* Block (via transport.wait) until a response may be available, or
 * timeout. The requester's idle wait when draining async responses.
 * Returns true if woken, false on timeout. (With the single-condvar
 * desktop transport this shares wakeups with the provider's wait;
 * each side re-polls its own ring, so spurious wakeups are benign.) */
bool channel_requester_wait(ServiceChannel *ch, uint32_t timeout_ms);

/* Post a request and block until the response with the matching
 * seq arrives or timeout_ms elapses. Convenience for sync calls;
 * handles seq stamping. Returns true and fills *out on response;
 * false on timeout or post failure. NOTE: assumes the requester is
 * the only one draining the response ring (true by endpoint
 * discipline); any responses for *other* seqs popped while waiting
 * are delivered via the optional `stray` callback so they are not
 * lost. Pass stray=NULL to drop them (only safe if all calls on
 * this channel are synchronous). */
typedef void (*ChannelStrayFn)(void *user, const ChannelMsg *resp);
bool channel_request_call(ServiceChannel *ch, ChannelMsg *msg /*in/out*/,
                          uint32_t timeout_ms,
                          ChannelStrayFn stray, void *stray_user);

/* ---- provider-side (service context) ---- */

/* Try to pop one request. Returns true if one was available. */
bool channel_request_poll(ServiceChannel *ch, ChannelMsg *out);

/* Post a response. Returns true on success, false if the response
 * ring is full. Notifies the requester on success. */
bool channel_response_post(ServiceChannel *ch, const ChannelMsg *msg);

/* Block (via transport.wait) until a request may be available, or
 * timeout. Provider's idle wait. Returns true if woken, false on
 * timeout. */
bool channel_provider_wait(ServiceChannel *ch, uint32_t timeout_ms);

#endif /* SERVICE_CHANNEL_H */
