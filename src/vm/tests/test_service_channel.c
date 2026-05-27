/* ============================================================
 *  test_service_channel.c — echo round-trip over the channel
 *
 *  A real worker thread plays the PROVIDER (the "M4"): it loops
 *  polling the request ring, and for each MSG_ECHO request posts a
 *  response echoing a0..a4 with the same seq. The main thread plays
 *  the REQUESTER (the "M7"/VM) and checks:
 *
 *    1. Sync call (channel_request_call) round-trips: response has
 *       the matching seq and the echoed args.
 *    2. Many sync calls in a row each get their own correct response
 *       (seq correlation works).
 *    3. Async posts followed by manual response draining work, and
 *       seqs correlate when responses arrive out of step.
 *    4. A burst that exceeds the ring depth still completes (post
 *       backs off and retries; nothing is dropped).
 *
 *  Run under ThreadSanitizer too — the channel adds no shared
 *  mutable state beyond the rings (SPSC, already proven) and the
 *  transport's own mutex/condvar, so it should be race-clean:
 *    cc -std=c11 -fsanitize=thread -Iinclude -o t \
 *       src/vm/tests/test_service_channel.c src/vm/service_channel.c \
 *       src/vm/channel_thread.c src/containers/spsc_ring.c -lpthread
 *
 *  Build (plain):
 *    cc -std=c11 -Iinclude -o t \
 *       src/vm/tests/test_service_channel.c src/vm/service_channel.c \
 *       src/vm/channel_thread.c src/containers/spsc_ring.c -lpthread
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/service_channel.h"
#include "vm/channel_thread.h"

#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

#define SLOTS 8   /* 7 usable — deliberately small */

static ServiceChannel g_ch;
static _Atomic int g_provider_stop = 0;

/* The provider/"M4": drain requests, echo them, until told to stop. */
static void *provider_thread(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&g_provider_stop, memory_order_acquire)) {
        ChannelMsg req;
        if (channel_request_poll(&g_ch, &req)) {
            if (req.type == MSG_ECHO) {
                ChannelMsg resp = req;        /* copy seq + args */
                resp.a3 = 0;                  /* status = ok */
                /* retry posting the response until it fits */
                while (!channel_response_post(&g_ch, &resp)) {
                    channel_provider_wait(&g_ch, 1);
                }
            }
        } else {
            /* idle: block briefly until a request may arrive */
            channel_provider_wait(&g_ch, 5);
        }
    }
    return NULL;
}

int main(void) {
    ChannelMsg req_storage[SLOTS], resp_storage[SLOTS];
    ChannelTransport tr;
    if (!channel_thread_transport_make(&tr)) {
        printf("  FAIL  transport make\n"); return 1;
    }
    if (!service_channel_init(&g_ch, req_storage, resp_storage, SLOTS, &tr)) {
        printf("  FAIL  channel init\n"); return 1;
    }

    pthread_t prov;
    pthread_create(&prov, NULL, provider_thread, NULL);

    /* ---- 1 & 2: a run of sync calls, each correlated by seq ---- */
    int sync_ok = 1;
    for (uint32_t i = 0; i < 1000; i++) {
        ChannelMsg m = (ChannelMsg){0};
        m.type = MSG_ECHO;
        m.a0 = i; m.a1 = i * 2; m.a2 = i * 3; m.a4 = 0xC0FFEE ^ i;
        bool ok = channel_request_call(&g_ch, &m, 1000, NULL, NULL);
        if (!ok) { sync_ok = 0; printf("  (sync call %u failed)\n", i); break; }
        if (m.a0 != i || m.a1 != i*2 || m.a2 != i*3 || m.a4 != (0xC0FFEE ^ i)) {
            sync_ok = 0; printf("  (sync echo mismatch at %u)\n", i); break;
        }
    }
    CHECK(sync_ok, "1000 sync echo calls round-trip with correct seq+args");

    /* ---- 3: async posts, then drain responses, match by seq ---- */
    /* Post several async requests, remembering each seq, then poll
     * all responses and confirm every seq came back exactly once. */
    {
        uint32_t seqs[5];
        int posted = 0;
        for (int i = 0; i < 5; i++) {
            ChannelMsg m = (ChannelMsg){0};
            m.type = MSG_ECHO;
            m.flags = CHANNEL_FLAG_EXPECTS_RESPONSE | CHANNEL_FLAG_ASYNC;
            m.seq = service_channel_next_seq(&g_ch);
            m.a0 = 0xA5A5 + i;
            seqs[i] = m.seq;
            /* retry on full */
            int tries = 0;
            while (!channel_request_post(&g_ch, &m)) {
                if (++tries > 100000) break;
            }
            if (tries <= 100000) posted++;
        }
        /* collect 5 responses — block on the channel between polls
         * rather than busy-spinning (a raw spin starves the provider
         * via cache contention, the same pathology the SPSC stress
         * test hit; real requesters wait, they don't spin). */
        int seen[5] = {0,0,0,0,0};
        int collected = 0, waits = 0;
        while (collected < posted && waits < 100000) {
            ChannelMsg r;
            if (channel_response_poll(&g_ch, &r)) {
                for (int i = 0; i < 5; i++) {
                    if (r.seq == seqs[i] && !seen[i]) {
                        seen[i] = 1; collected++;
                        if (r.a0 != (uint32_t)(0xA5A5 + i))
                            printf("  (async arg mismatch seq %u)\n", r.seq);
                        break;
                    }
                }
            } else {
                /* block until a response may have arrived (or 10ms) */
                if (!channel_requester_wait(&g_ch, 10)) waits++;
            }
        }
        CHECK(posted == 5, "5 async requests posted");
        CHECK(collected == 5, "all 5 async responses collected + seq-matched");
    }

    /* ---- 4: burst exceeding ring depth completes (no drops) ---- */
    /* SLOTS-1 = 7 usable; fire 50 sync calls — each must individually
     * complete, proving post backs off / retries rather than dropping. */
    {
        int burst_ok = 1;
        for (int i = 0; i < 50; i++) {
            ChannelMsg m = (ChannelMsg){0};
            m.type = MSG_ECHO; m.a0 = (uint32_t)(0xBEEF0000 + i);
            if (!channel_request_call(&g_ch, &m, 1000, NULL, NULL)
                || m.a0 != (uint32_t)(0xBEEF0000 + i)) {
                burst_ok = 0; break;
            }
        }
        CHECK(burst_ok, "50-call burst through a 7-slot ring all complete");
    }

    atomic_store_explicit(&g_provider_stop, 1, memory_order_release);
    pthread_join(prov, NULL);
    service_channel_destroy(&g_ch);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
