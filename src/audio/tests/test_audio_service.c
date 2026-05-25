/* ============================================================
 *  test_audio_service.c — the audio service over the channel
 *
 *  Two parts:
 *   A) Single-threaded: post requests, call audio_service_process
 *      directly, assert exact outcomes (alloc -> handle, trigger ->
 *      voice, free, reject-on-full, render non-silent). Deterministic.
 *   B) Threaded: a worker runs audio_service_run as the PROVIDER; the
 *      main thread posts requests as a VM would and reads responses,
 *      proving the real channel->service path end to end.
 *
 *  Build (single + threaded both need pthread for B):
 *    cc -std=c11 -Iinclude -o t \
 *       src/audio/tests/test_audio_service.c src/audio/audio_service.c \
 *       src/audio/audio_arbiter.c src/audio/audio_pool.c \
 *       src/audio/audio_pool_stream.c src/audio/audio_mixer.c \
 *       src/audio/music_player.c src/containers/ring_buffer.c \
 *       src/containers/spsc_ring.c src/vm/service_channel.c \
 *       src/vm/channel_thread.c -lpthread
 * ============================================================ */

#include "audio/audio_service.h"
#include "vm/channel_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

#define SLOTS 16
#define POOL_BYTES (256 * 1024)

/* ---- helpers to build requests ---- */
static ChannelMsg req(uint16_t type, uint32_t a0, uint32_t a1,
                      uint32_t a2, uint32_t a3) {
    ChannelMsg m; memset(&m, 0, sizeof(m));
    m.type = type; m.flags = CHANNEL_FLAG_EXPECTS_RESPONSE;
    m.a0 = a0; m.a1 = a1; m.a2 = a2; m.a3 = a3;
    return m;
}

/* ---- mock file reader + synthetic WAV (for the streaming voice) ---- */
typedef struct { const uint8_t *buf; uint32_t len; uint32_t pos; } SvcMemFile;
static void *svc_mf_open(void *ctx, const char *path) {
    (void)path; SvcMemFile *mf = (SvcMemFile *)ctx; mf->pos = 0; return mf;
}
static uint32_t svc_mf_read(void *ctx, void *fh, void *dst, uint32_t bytes) {
    (void)ctx; SvcMemFile *mf = (SvcMemFile *)fh;
    uint32_t avail = mf->len - mf->pos, n = bytes < avail ? bytes : avail;
    memcpy(dst, mf->buf + mf->pos, n); mf->pos += n; return n;
}
static bool svc_mf_seek(void *ctx, void *fh, uint32_t off) {
    (void)ctx; SvcMemFile *mf = (SvcMemFile *)fh;
    if (off > mf->len) return false;
    mf->pos = off;
    return true;
}
static void svc_mf_close(void *ctx, void *fh) { (void)ctx; (void)fh; }

static uint32_t put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);return 4;
}
static uint32_t put16(uint8_t *p, uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);return 2;}

/* ============================================================
 *  Part C — file-stream music voice (REQ_AUDIO_STREAM_WAV)
 *
 *  End-to-end through the service: a mock AudioFileReader serves a
 *  synthetic STEREO wav from memory, the service opens it on STREAM_WAV
 *  and renders non-silent stereo output (L != R proves stereo is
 *  preserved, not downmixed). The desktop stdio/FatFs readers feed this
 *  exact path — only the reader differs.
 * ============================================================ */
static void test_stream_wav(void) {
    static uint8_t wav[64 * 1024];
    const uint32_t nf = 4000, rate = 44100; const int ch = 2;
    uint32_t data = nf * (uint32_t)ch * 2u, p = 0;
    memcpy(wav+p,"RIFF",4);p+=4;p+=put32(wav+p,36+data);memcpy(wav+p,"WAVE",4);p+=4;
    memcpy(wav+p,"fmt ",4);p+=4;p+=put32(wav+p,16);p+=put16(wav+p,1);
    p+=put16(wav+p,(uint16_t)ch);p+=put32(wav+p,rate);
    p+=put32(wav+p,rate*(uint32_t)ch*2u);p+=put16(wav+p,(uint16_t)(ch*2));p+=put16(wav+p,16);
    memcpy(wav+p,"data",4);p+=4;p+=put32(wav+p,data);
    for (uint32_t i = 0; i < nf; i++) {           /* distinct L/R ramps */
        int16_t l = (int16_t)(8000 - (int)(i % 2000));
        int16_t r = (int16_t)((int)(i % 2000) - 8000);
        p += put16(wav+p,(uint16_t)l); p += put16(wav+p,(uint16_t)r);
    }
    SvcMemFile mf = { wav, p, 0 };

    ChannelMsg rqs[SLOTS], rss[SLOTS];
    ChannelTransport tr; channel_thread_transport_make(&tr);
    ServiceChannel ch2; service_channel_init(&ch2, rqs, rss, SLOTS, &tr);
    uint8_t *region = malloc(POOL_BYTES);
    static uint8_t staging[8192];
    AudioServiceConfig cfg = {
        .channel = &ch2, .pool_region = region, .pool_region_size = POOL_BYTES,
        .sample_rate = 44100, .track_count = 4,
        .staging_buffer = staging, .staging_capacity = sizeof(staging),
        .file_reader = { svc_mf_open, svc_mf_read, svc_mf_seek, svc_mf_close, &mf },
    };
    AudioService *svc = audio_service_create(&cfg);
    CHECK(svc != NULL, "service (stream) created");

    /* stage a path (the mock reader ignores it) and post STREAM_WAV */
    const char *path = "0:/song.wav"; uint32_t plen = (uint32_t)strlen(path) + 1;
    memcpy(staging, path, plen);
    ChannelMsg sm = req(REQ_AUDIO_STREAM_WAV, plen, /*vm*/1, 0, 0);
    sm.seq = service_channel_next_seq(&ch2);
    CHECK(channel_request_post(&ch2, &sm), "post STREAM_WAV");
    CHECK(audio_service_process(svc, 8) == 1, "service handled STREAM_WAV");
    ChannelMsg sr; CHECK(channel_response_poll(&ch2, &sr), "STREAM_WAV response");
    CHECK(sr.a3 == AUDIO_ARB_OK, "STREAM_WAV OK");
    AudioVoiceHandle v = sr.a4;
    CHECK(v != 0, "stream returned a voice");
    CHECK(audio_arbiter_active_count(audio_service_arbiter(svc)) == 1,
          "one active stream voice");

    int16_t out[256 * 2]; memset(out, 0, sizeof out);
    for (int k = 0; k < 3; k++) {
        audio_service_process(svc, 1);          /* pumps the stream */
        audio_service_render(svc, out, 256);
    }
    int nz = 0, lr_diff = 0;
    for (int i = 0; i < 256; i++) {
        if (out[i*2] || out[i*2+1]) nz++;
        if (out[i*2] != out[i*2+1]) lr_diff++;
    }
    CHECK(nz > 0, "streamed wav reached the output (non-silent)");
    CHECK(lr_diff > 0, "stereo preserved through the stream (L != R)");

    /* Mute regression: SET_GAIN on a streaming voice must be accepted
     * and actually silence the output. A streaming voice has no pool
     * object, so the old object-existence gate rejected it (BAD_VOICE)
     * and the [M]ute key in musicplayer no-op'd. */
    ChannelMsg gr, gain = req(REQ_AUDIO_SET_GAIN, v, /*gain q15*/0, 0, 0);
    gain.seq = service_channel_next_seq(&ch2);
    channel_request_post(&ch2, &gain); audio_service_process(svc, 8);
    channel_response_poll(&ch2, &gr);
    CHECK(gr.a3 == AUDIO_ARB_OK, "SET_GAIN accepted on a streaming voice");
    memset(out, 0, sizeof out);
    for (int k = 0; k < 3; k++) {
        audio_service_process(svc, 1);
        audio_service_render(svc, out, 256);
    }
    int nz_muted = 0;
    for (int i = 0; i < 256 * 2; i++) if (out[i]) nz_muted++;
    CHECK(nz_muted == 0, "gain=0 silences the streaming voice (mute works)");

    ChannelMsg st, stop = req(REQ_AUDIO_STOP_MUSIC, v, 0, 0, 0);
    stop.seq = service_channel_next_seq(&ch2);
    channel_request_post(&ch2, &stop); audio_service_process(svc, 8);
    channel_response_poll(&ch2, &st);
    CHECK(st.a3 == AUDIO_ARB_OK, "stop stream voice OK");
    CHECK(audio_arbiter_active_count(audio_service_arbiter(svc)) == 0,
          "stream voice gone after stop");

    audio_service_destroy(svc);
    service_channel_destroy(&ch2);
    free(region);
}

/* ============================================================
 *  Part A — single-threaded, deterministic
 * ============================================================ */
static void test_single_threaded(void) {
    ChannelMsg rqs[SLOTS], rss[SLOTS];
    ChannelTransport tr; channel_thread_transport_make(&tr);
    ServiceChannel ch;
    service_channel_init(&ch, rqs, rss, SLOTS, &tr);

    uint8_t *region = malloc(POOL_BYTES);
    static uint8_t staging[8192];
    AudioServiceConfig cfg = { .channel = &ch, .pool_region = region,
                               .pool_region_size = POOL_BYTES,
                               .sample_rate = 44100, .track_count = 4,
                               .staging_buffer = staging,
                               .staging_capacity = sizeof(staging) };
    AudioService *svc = audio_service_create(&cfg);
    CHECK(svc != NULL, "service created");

    /* STAGED LOAD: put a known pattern in staging, post LOAD_STAGED,
     * verify the returned object holds it. */
    {
        uint32_t n = 1024;
        for (uint32_t i = 0; i < n; i++) staging[i] = (uint8_t)(i * 7 + 3);
        ChannelMsg lm = req(REQ_AUDIO_LOAD_STAGED, n, 1, 0, 0);
        lm.seq = service_channel_next_seq(&ch);
        channel_request_post(&ch, &lm);
        audio_service_process(svc, 8);
        ChannelMsg lr;
        channel_response_poll(&ch, &lr);
        CHECK(lr.a3 == AUDIO_POOL_OK, "staged load OK");
        AudioObjHandle lh = lr.a4;
        CHECK(lh != 0, "staged load returned a handle");
        /* read it back from the pool and compare */
        uint8_t back[1024];
        uint32_t got = 0;
        audio_pool_read(audio_service_pool(svc), lh, 0, back, n, &got);
        CHECK(got == n, "loaded object full size");
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) if (back[i] != (uint8_t)(i*7+3)) ok = 0;
        CHECK(ok, "staged PCM copied into pool object intact");
        audio_pool_unref(audio_service_pool(svc), lh, NULL);
    }

    /* helper: post a request, process it, pop the response */
    #define ROUNDTRIP(M, RESP) do {                                       \
        ChannelMsg _m = (M); _m.seq = service_channel_next_seq(&ch);      \
        CHECK(channel_request_post(&ch, &_m), "post request");            \
        CHECK(audio_service_process(svc, 8) == 1, "service handled one"); \
        CHECK(channel_response_poll(&ch, &(RESP)), "got response");       \
    } while (0)

    /* ALLOC an object */
    ChannelMsg r1;
    ROUNDTRIP(req(REQ_AUDIO_POOL_ALLOC, 1000, /*vm*/1, 0, 0), r1);
    CHECK(r1.a3 == AUDIO_POOL_OK, "alloc status OK");
    AudioObjHandle obj = r1.a4;
    CHECK(obj != 0, "alloc returned a handle");

    /* TRIGGER it -> voice */
    ChannelMsg r2;
    ROUNDTRIP(req(REQ_AUDIO_TRIGGER_SFX, obj, /*gain*/32767, /*pan*/0, /*vm*/1), r2);
    CHECK(r2.a3 == AUDIO_ARB_OK, "trigger status OK");
    AudioVoiceHandle voice = r2.a4;
    CHECK(voice != 0, "trigger returned a voice");
    CHECK(audio_arbiter_active_count(audio_service_arbiter(svc)) == 1,
          "one active voice in arbiter");

    /* The trigger took a pool ref (creator + voice = 2). */
    CHECK(audio_pool_refcount(audio_service_pool(svc), obj) == 2,
          "playing took a pool ref");

    /* RENDER — output should be touched (we wrote a nonzero-ish ramp?
     * The object data is uninitialized pool memory, so just confirm
     * render runs without crashing and produces frames). */
    int16_t out[128 * 2];
    memset(out, 0, sizeof(out));
    audio_service_render(svc, out, 128);
    CHECK(1, "render completed");

    /* STOP the voice -> frees track, drops ref */
    ChannelMsg r3;
    ROUNDTRIP(req(REQ_AUDIO_VOICE_STOP, voice, 0, 0, 0), r3);
    CHECK(r3.a3 == AUDIO_ARB_OK, "stop status OK");
    CHECK(audio_arbiter_active_count(audio_service_arbiter(svc)) == 0,
          "no active voices after stop");
    CHECK(audio_pool_refcount(audio_service_pool(svc), obj) == 1,
          "voice ref dropped on stop");

    /* Fill all 4 tracks, then the 5th trigger is REJECTED. */
    AudioObjHandle o2; ChannelMsg ra;
    ROUNDTRIP(req(REQ_AUDIO_POOL_ALLOC, 500, 1, 0, 0), ra); o2 = ra.a4;
    AudioVoiceHandle fillv[4];
    for (int i = 0; i < 4; i++) {
        ChannelMsg rt;
        ROUNDTRIP(req(REQ_AUDIO_TRIGGER_SFX, o2, 32767, 0, 1), rt);
        CHECK(rt.a3 == AUDIO_ARB_OK, "fill track via trigger");
        fillv[i] = rt.a4;
    }
    ChannelMsg rrej;
    ROUNDTRIP(req(REQ_AUDIO_TRIGGER_SFX, o2, 32767, 0, 1), rrej);
    CHECK(rrej.a3 == AUDIO_ARB_REJECTED, "5th trigger REJECTED (4 tracks)");
    CHECK(rrej.a4 == 0, "rejected -> no voice");

    /* Free the filled tracks again so later sections have room. */
    for (int i = 0; i < 4; i++) {
        ChannelMsg rt; ROUNDTRIP(req(REQ_AUDIO_VOICE_STOP, fillv[i], 0, 0, 0), rt);
    }
    { ChannelMsg t; ROUNDTRIP(req(REQ_AUDIO_POOL_FREE, o2, 0, 0, 0), t); }

    /* FREE the original object (creator drops its ref). */
    ChannelMsg rf;
    ROUNDTRIP(req(REQ_AUDIO_POOL_FREE, obj, 0, 0, 0), rf);
    CHECK(rf.a3 == AUDIO_POOL_OK, "free status OK");
    CHECK(rf.a4 == 1, "free reported object freed (refcount hit 0)");

    /* trigger a bad object -> BAD_OBJECT */
    ChannelMsg rbad;
    ROUNDTRIP(req(REQ_AUDIO_TRIGGER_SFX, 0xDEADBEEF, 32767, 0, 1), rbad);
    CHECK(rbad.a3 == AUDIO_ARB_BAD_OBJECT, "trigger bad object -> BAD_OBJECT");

    /* ---- MUSIC PATH ---- */
    {
        /* Load two sample objects (intro + loop) via staged load. */
        uint32_t n = 2048;
        for (uint32_t i = 0; i < n; i++) staging[i] = (uint8_t)(i & 0xFF);
        ChannelMsg ri; ROUNDTRIP(req(REQ_AUDIO_LOAD_STAGED, n, 1, 0, 0), ri);
        AudioObjHandle intro = ri.a4;
        ChannelMsg rl; ROUNDTRIP(req(REQ_AUDIO_LOAD_STAGED, n, 1, 0, 0), rl);
        AudioObjHandle loop = rl.a4;
        CHECK(intro != 0 && loop != 0, "loaded intro + loop sample objects");

        /* Pair them into a music object. */
        ChannelMsg rm;
        ROUNDTRIP(req(REQ_AUDIO_LOAD_MUSIC, intro, loop, 1, 0), rm);
        CHECK(rm.a3 == AUDIO_POOL_OK, "LOAD_MUSIC OK");
        AudioObjHandle music = rm.a4;
        CHECK(music != 0, "got a music handle");
        CHECK((music & 0x80000000u) != 0, "music handle is tagged");

        /* intro+loop now each have 2 refs (creator + music object). */
        CHECK(audio_pool_refcount(audio_service_pool(svc), intro) == 2,
              "music object holds a ref on intro");
        CHECK(audio_pool_refcount(audio_service_pool(svc), loop) == 2,
              "music object holds a ref on loop");

        /* Play the music object -> a voice on a music-capable track. */
        ChannelMsg rp;
        ROUNDTRIP(req(REQ_AUDIO_PLAY_MUSIC, music, 0, 0, 1), rp);
        CHECK(rp.a3 == AUDIO_ARB_OK, "PLAY_MUSIC OK (no longer rejected)");
        AudioVoiceHandle mvoice = rp.a4;
        CHECK(mvoice != 0, "music play returned a voice");
        CHECK(audio_arbiter_active_count(audio_service_arbiter(svc)) == 1,
              "one active music voice");

        /* Pump + render: the music player should have fed its mixer
         * channel, so render produces output. */
        int16_t out[256 * 2];
        memset(out, 0, sizeof(out));
        audio_service_process(svc, 1);   /* pumps music */
        audio_service_render(svc, out, 256);
        int nz = 0; for (int i = 0; i < 256*2; i++) if (out[i] != 0) nz++;
        CHECK(nz > 0, "music reached the mixer output (non-silent)");

        /* Stop the music voice -> releases the player + loop ref. */
        ChannelMsg rs2;
        ROUNDTRIP(req(REQ_AUDIO_STOP_MUSIC, mvoice, 0, 0, 0), rs2);
        CHECK(rs2.a3 == AUDIO_ARB_OK, "stop music OK");
        CHECK(audio_arbiter_active_count(audio_service_arbiter(svc)) == 0,
              "music voice gone after stop");
        /* loop ref dropped back to 2 (the playing ref is gone). The
         * arbiter held intro; that's also dropped. So both back to the
         * music object's single ref + the creator's. */
        CHECK(audio_pool_refcount(audio_service_pool(svc), loop) == 2,
              "loop play-ref released on stop");

        /* Free the music object -> drops its refs on intro/loop. */
        ChannelMsg rmf;
        ROUNDTRIP(req(REQ_AUDIO_POOL_FREE, music, 0, 0, 0), rmf);
        CHECK(rmf.a3 == AUDIO_POOL_OK, "free music object OK");
        CHECK(audio_pool_refcount(audio_service_pool(svc), intro) == 1,
              "intro back to creator ref after music freed");
        /* clean up the creator refs */
        ChannelMsg t;
        ROUNDTRIP(req(REQ_AUDIO_POOL_FREE, intro, 0, 0, 0), t);
        ROUNDTRIP(req(REQ_AUDIO_POOL_FREE, loop, 0, 0, 0), t);
    }

    /* ---- FFT band meter plumbing ---- */
    {
        /* Disabled by default: GET_LEVELS returns 0 bands. */
        ChannelMsg rg0;
        ROUNDTRIP(req(REQ_AUDIO_GET_LEVELS, 0, 0, 0, 0), rg0);
        CHECK(rg0.a4 == 0, "GET_LEVELS returns 0 bands while disabled");

        /* Enable. */
        ChannelMsg re;
        ROUNDTRIP(req(REQ_AUDIO_FFT_ENABLE, 1, 0, 0, 0), re);
        CHECK(re.a3 == AUDIO_ARB_OK, "FFT enable OK");

        /* Render enough output to fill a window, processing between
         * renders so update() runs. Output is silence here (no voice),
         * so bands should be ~0 but the count should be the full set. */
        int16_t out[256 * 2];
        for (int i = 0; i < 4; i++) {
            audio_service_render(svc, out, 256);   /* captures */
            audio_service_process(svc, 1);         /* updates  */
        }
        ChannelMsg rg;
        ROUNDTRIP(req(REQ_AUDIO_GET_LEVELS, 0, 0, 0, 0), rg);
        CHECK(rg.a4 == 16, "GET_LEVELS returns 16 bands when enabled");

        /* Disable again. */
        ChannelMsg rd;
        ROUNDTRIP(req(REQ_AUDIO_FFT_ENABLE, 0, 0, 0, 0), rd);
        ChannelMsg rg2;
        ROUNDTRIP(req(REQ_AUDIO_GET_LEVELS, 0, 0, 0, 0), rg2);
        CHECK(rg2.a4 == 0, "GET_LEVELS returns 0 bands after disable");
    }

    /* ---- FFT enable is refcounted across consumers (multi-app) ---- */
    {
        int16_t out[256 * 2];
        #define PUMP() do { for (int i = 0; i < 4; i++) {                  \
            audio_service_render(svc, out, 256);                           \
            audio_service_process(svc, 1); } } while (0)

        ChannelMsg r;
        ROUNDTRIP(req(REQ_AUDIO_FFT_ENABLE, 1, /*vm*/1, 0, 0), r);  /* app 1 on */
        ROUNDTRIP(req(REQ_AUDIO_FFT_ENABLE, 1, /*vm*/2, 0, 0), r);  /* app 2 on */
        PUMP();
        ChannelMsg g1; ROUNDTRIP(req(REQ_AUDIO_GET_LEVELS, 0, 0, 0, 0), g1);
        CHECK(g1.a4 == 16, "two consumers -> meter on");

        /* app 1 quits (disables). The meter MUST stay on for app 2 — this
         * is the bug fix (a global toggle would blank app 2's equalizer). */
        ROUNDTRIP(req(REQ_AUDIO_FFT_ENABLE, 0, /*vm*/1, 0, 0), r);
        PUMP();
        ChannelMsg g2; ROUNDTRIP(req(REQ_AUDIO_GET_LEVELS, 0, 0, 0, 0), g2);
        CHECK(g2.a4 == 16, "one consumer leaves -> meter stays on for the other");

        /* app 2 dies without disabling (disconnect). The host's VM-teardown
         * hook posts REQ_AUDIO_SWEEP_VM, which releases its hold so the
         * meter doesn't leak on forever. Drive it through the channel (not
         * the direct call) to cover the dispatch the unload hook uses. */
        ChannelMsg sw; ROUNDTRIP(req(REQ_AUDIO_SWEEP_VM, /*vm*/2, 0, 0, 0), sw);
        CHECK(sw.a3 == (uint32_t)AUDIO_ARB_OK, "sweep request acked");
        ChannelMsg g3; ROUNDTRIP(req(REQ_AUDIO_GET_LEVELS, 0, 0, 0, 0), g3);
        CHECK(g3.a4 == 0, "last consumer swept -> meter off (no leak)");
        #undef PUMP
    }

    #undef ROUNDTRIP
    audio_service_destroy(svc);
    service_channel_destroy(&ch);   /* frees the transport ctx */
    free(region);
}

/* ============================================================
 *  Part B — threaded provider, real channel round-trip
 * ============================================================ */
static AudioService     *gb_svc;
static ServiceChannel    gb_ch;
static _Atomic int       gb_stop = 0;

static bool stopper(void *u) { (void)u; return atomic_load_explicit(&gb_stop, memory_order_acquire); }
static void *provider(void *u) { (void)u; audio_service_run(gb_svc, stopper, NULL); return NULL; }

/* sync call: post + wait for the matching seq response */
static bool call(uint16_t type, uint32_t a0, uint32_t a1, uint32_t a2,
                 uint32_t a3, ChannelMsg *out) {
    ChannelMsg m = req(type, a0, a1, a2, a3);
    return channel_request_call(&gb_ch, &m, 1000, NULL, NULL) ? (*out = m, true) : false;
}

static void test_threaded(void) {
    ChannelMsg rqs[SLOTS], rss[SLOTS];
    ChannelTransport tr; channel_thread_transport_make(&tr);
    service_channel_init(&gb_ch, rqs, rss, SLOTS, &tr);

    uint8_t *region = malloc(POOL_BYTES);
    AudioServiceConfig cfg = { .channel = &gb_ch, .pool_region = region,
                               .pool_region_size = POOL_BYTES,
                               .sample_rate = 44100, .track_count = 8 };
    gb_svc = audio_service_create(&cfg);
    CHECK(gb_svc != NULL, "threaded: service created");

    pthread_t th;
    atomic_store(&gb_stop, 0);
    pthread_create(&th, NULL, provider, NULL);

    /* alloc -> trigger -> stop, all via real channel round-trips */
    ChannelMsg r;
    CHECK(call(REQ_AUDIO_POOL_ALLOC, 2000, 1, 0, 0, &r), "threaded alloc round-trip");
    CHECK(r.a3 == AUDIO_POOL_OK, "threaded alloc OK");
    AudioObjHandle obj = r.a4;

    CHECK(call(REQ_AUDIO_TRIGGER_SFX, obj, 32767, 0, 1, &r), "threaded trigger round-trip");
    CHECK(r.a3 == AUDIO_ARB_OK, "threaded trigger OK");
    AudioVoiceHandle v = r.a4;

    CHECK(call(REQ_AUDIO_VOICE_STOP, v, 0, 0, 0, &r), "threaded stop round-trip");
    CHECK(r.a3 == AUDIO_ARB_OK, "threaded stop OK");

    CHECK(call(REQ_AUDIO_POOL_FREE, obj, 0, 0, 0, &r), "threaded free round-trip");
    CHECK(r.a3 == AUDIO_POOL_OK, "threaded free OK");

    atomic_store_explicit(&gb_stop, 1, memory_order_release);
    pthread_join(th, NULL);
    audio_service_destroy(gb_svc);
    service_channel_destroy(&gb_ch);   /* frees the transport ctx */
    free(region);
}

int main(void) {
    test_single_threaded();
    test_stream_wav();
    test_threaded();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
