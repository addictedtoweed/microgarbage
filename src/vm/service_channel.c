/* ============================================================
 *  service_channel.c — full-duplex channel over two SPSC rings
 *
 *  The rings carry messages; the transport only wakes/blocks peers.
 *  See service_channel.h for the endpoint discipline that keeps each
 *  ring single-producer/single-consumer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/service_channel.h"

#include <string.h>

/* ---- lifecycle ---- */

bool service_channel_init(ServiceChannel *ch,
                          void *req_storage, void *resp_storage,
                          size_t slots,
                          const ChannelTransport *transport) {
    if (!ch || !req_storage || !resp_storage || !transport) return false;

    if (!spsc_ring_init(&ch->req, req_storage, slots, sizeof(ChannelMsg)))
        return false;
    if (!spsc_ring_init(&ch->resp, resp_storage, slots, sizeof(ChannelMsg)))
        return false;

    ch->transport = *transport;
    atomic_store_explicit(&ch->next_seq, 1, memory_order_relaxed);
    return true;
}

void service_channel_destroy(ServiceChannel *ch) {
    if (!ch) return;
    if (ch->transport.destroy) ch->transport.destroy(ch->transport.ctx);
    ch->transport.notify  = NULL;
    ch->transport.wait    = NULL;
    ch->transport.destroy = NULL;
    ch->transport.ctx     = NULL;
}

uint32_t service_channel_next_seq(ServiceChannel *ch) {
    if (!ch) return 0;
    uint32_t s = atomic_fetch_add_explicit(&ch->next_seq, 1,
                                           memory_order_relaxed);
    if (s == 0) {   /* never hand out 0 (reserved as "no seq") */
        s = atomic_fetch_add_explicit(&ch->next_seq, 1,
                                      memory_order_relaxed);
    }
    return s;
}

/* ---- requester side ---- */

bool channel_request_post(ServiceChannel *ch, const ChannelMsg *msg) {
    if (!ch || !msg) return false;
    if (!spsc_ring_push(&ch->req, msg)) return false;
    if (ch->transport.notify) ch->transport.notify(ch->transport.ctx);
    return true;
}

bool channel_response_poll(ServiceChannel *ch, ChannelMsg *out) {
    if (!ch || !out) return false;
    return spsc_ring_pop(&ch->resp, out);
}

bool channel_requester_wait(ServiceChannel *ch, uint32_t timeout_ms) {
    if (!ch || !ch->transport.wait) return false;
    return ch->transport.wait(ch->transport.ctx, timeout_ms);
}

bool channel_request_call(ServiceChannel *ch, ChannelMsg *msg,
                          uint32_t timeout_ms,
                          ChannelStrayFn stray, void *stray_user) {
    if (!ch || !msg) return false;

    /* Stamp a fresh seq and require a response. */
    uint32_t seq = service_channel_next_seq(ch);
    msg->seq    = seq;
    msg->flags  = (uint16_t)(msg->flags | CHANNEL_FLAG_EXPECTS_RESPONSE);
    msg->flags  = (uint16_t)(msg->flags & ~CHANNEL_FLAG_ASYNC);

    /* Post (retry-on-full with the transport wait as backoff). */
    while (!channel_request_post(ch, msg)) {
        if (!ch->transport.wait) return false;       /* no way to block */
        if (!ch->transport.wait(ch->transport.ctx, timeout_ms)) {
            return false;                            /* timed out posting */
        }
    }

    /* Wait for the response matching `seq`. Any other response popped
     * meanwhile is a stray (an async reply or another sync call's
     * response) — hand it to the callback so it isn't lost. */
    for (;;) {
        ChannelMsg r;
        bool got = channel_response_poll(ch, &r);
        if (got) {
            if (r.seq == seq) {
                *msg = r;
                return true;
            }
            if (stray) stray(stray_user, &r);
            /* else dropped — caller asserted all-sync usage */
            continue;   /* keep draining for our seq */
        }
        /* Nothing yet — block until notified or timeout. */
        if (!ch->transport.wait) return false;
        if (!ch->transport.wait(ch->transport.ctx, timeout_ms)) {
            return false;   /* timeout waiting for our response */
        }
    }
}

/* ---- provider side ---- */

bool channel_request_poll(ServiceChannel *ch, ChannelMsg *out) {
    if (!ch || !out) return false;
    return spsc_ring_pop(&ch->req, out);
}

bool channel_response_post(ServiceChannel *ch, const ChannelMsg *msg) {
    if (!ch || !msg) return false;
    if (!spsc_ring_push(&ch->resp, msg)) return false;
    if (ch->transport.notify) ch->transport.notify(ch->transport.ctx);
    return true;
}

bool channel_provider_wait(ServiceChannel *ch, uint32_t timeout_ms) {
    if (!ch || !ch->transport.wait) return false;
    return ch->transport.wait(ch->transport.ctx, timeout_ms);
}
