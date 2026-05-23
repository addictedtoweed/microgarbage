/* ============================================================
 *  audio_service.c — the audio co-processor service
 *
 *  Drains REQ_AUDIO_* from the channel and drives pool + arbiter +
 *  mixer. See audio_service.h.
 *
 *  This first cut implements the SFX path end to end (pool -> mixer
 *  channel -> render) and the pool/lifetime requests (alloc, free,
 *  load, ref via play, sweep). The MUSIC path (intro+loop via a
 *  music_player instance per track, fed by the pool adapter) is
 *  stubbed with a clear marker — it needs a music_player pool and the
 *  per-track adapter binding, wired in a follow-up. SFX proves the
 *  whole channel->service->pool->arbiter->mixer->render chain.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_service.h"
#include "audio/audio_fft.h"

#include <stdlib.h>
#include <string.h>

/* Per-music-player buffer sizes (frames). The streaming buffer must
 * be comfortably larger than one render quantum; the pinned heads
 * cover the intro->loop and loop->loop seams. Mono16 here. */
#define MUSIC_STREAM_FRAMES   8192
#define MUSIC_HEAD_FRAMES     1024

/* A music object pairs an intro pool object with a loop pool object
 * (the two-file model). Music handles are tagged (high bit set) to
 * distinguish them from sample object handles at the ABI. */
#define MUSIC_HANDLE_TAG   0x80000000u
#define MUSIC_MAX_OBJECTS  16

typedef struct {
    bool           in_use;
    AudioObjHandle intro;     /* pool object (refcounted) */
    AudioObjHandle loop;      /* pool object, or NONE for intro-only */
    uint16_t       owner_vm;
} MusicObject;

/* A music_player instance bound to a mixer channel, used while a
 * music voice is playing. One per concurrent music stream. */
typedef struct {
    bool                in_use;
    uint32_t            track;       /* mixer channel it feeds */
    MusicPlayer        *player;
    AudioPoolStreamCtx  stream_ctx;  /* binds pool objects -> player */
    /* per-instance buffers (contiguous SRAM staging, NOT block pool) */
    int16_t            *streaming_buf;
    int16_t            *intro_head;
    int16_t            *loop_head;
} MusicSlot;

struct AudioService {
    ServiceChannel *channel;
    AudioPool       pool;
    AudioArbiter    arbiter;
    AudioMixer     *mixer;
    uint32_t        sample_rate;
    uint32_t        track_count;

    /* shared staging buffer for staged loads */
    uint8_t        *staging;
    size_t          staging_cap;

    /* music object table (handle -> intro/loop pool object pair) */
    MusicObject     music_objs[MUSIC_MAX_OBJECTS];
    uint16_t        music_gen[MUSIC_MAX_OBJECTS];

    /* music player instances (one per concurrent music stream) */
    MusicSlot       music_slots[AUDIO_SERVICE_MAX_MUSIC];

    /* "pending music play" — set by handle_one before calling
     * audio_arbiter_play(kind=MUSIC) so the sink (which only gets the
     * arbiter `object`) can reach the intro/loop pair + music handle.
     * Valid only across a single play call (single-context). */
    AudioObjHandle  pend_intro;
    AudioObjHandle  pend_loop;

    /* FFT band meter over the final mixed output (enable-gated). */
    AudioFft        fft;

    /* Scratch buffer for moving PCM from the pool into a mixer
     * channel on SFX start. Sized to one block. */
    uint8_t         scratch[AUDIO_POOL_BLOCK_SIZE];
};

/* ---- music object handle pack/unpack (tagged, generation) ---- */

static AudioObjHandle pack_music(uint32_t slot, uint16_t gen) {
    return MUSIC_HANDLE_TAG
         | (((AudioObjHandle)(gen & 0x7FFFu)) << 16)
         | ((slot + 1u) & 0x7FFFu);
}
static bool is_music_handle(AudioObjHandle h) {
    return (h & MUSIC_HANDLE_TAG) != 0;
}
static bool unpack_music(AudioObjHandle h, uint32_t *slot, uint16_t *gen) {
    if (!(h & MUSIC_HANDLE_TAG)) return false;
    uint32_t s = (h & 0x7FFFu);
    if (s == 0) return false;
    *slot = s - 1u;
    *gen  = (uint16_t)((h >> 16) & 0x7FFFu);
    return true;
}

/* Resolve a music handle to its table entry, or NULL. */
static MusicObject *resolve_music(AudioService *svc, AudioObjHandle h) {
    uint32_t slot; uint16_t gen;
    if (!unpack_music(h, &slot, &gen)) return NULL;
    if (slot >= MUSIC_MAX_OBJECTS) return NULL;
    MusicObject *mo = &svc->music_objs[slot];
    if (!mo->in_use) return NULL;
    if ((svc->music_gen[slot] & 0x7FFFu) != gen) return NULL;
    return mo;
}

/* Find a free music_player slot (one per concurrent stream). */
static MusicSlot *find_free_music_slot(AudioService *svc) {
    for (uint32_t i = 0; i < AUDIO_SERVICE_MAX_MUSIC; i++) {
        if (!svc->music_slots[i].in_use) return &svc->music_slots[i];
    }
    return NULL;
}
static MusicSlot *music_slot_for_track(AudioService *svc, uint32_t track) {
    for (uint32_t i = 0; i < AUDIO_SERVICE_MAX_MUSIC; i++) {
        if (svc->music_slots[i].in_use && svc->music_slots[i].track == track)
            return &svc->music_slots[i];
    }
    return NULL;
}

/* ---- arbiter sink: start/stop drive the mixer ---- */

/* The sink ctx is the service itself. On SFX start we pull the
 * object's PCM from the pool and feed it into the track's mixer
 * channel, then start the channel. */
static bool svc_sink_start(void *ctx, uint32_t track, AudioVoiceKind kind,
                           AudioObjHandle object, const AudioVoiceParams *p) {
    AudioService *svc = (AudioService *)ctx;

    if (kind == AUDIO_VOICE_MUSIC) {
        /* `object` is the INTRO pool object (the arbiter reffed it).
         * The loop object is in svc->pend_loop (handle_one set it).
         * Grab a free music_player slot, bind intro/loop via the pool
         * adapter, create the player on this mixer channel, prime,
         * play. We take an extra ref on the loop object here (intro is
         * already held by the arbiter); both released on stop. */
        AudioObjHandle intro = object;
        AudioObjHandle loop  = svc->pend_loop;

        MusicSlot *ms = find_free_music_slot(svc);
        if (!ms) return false;     /* no free music stream slot */

        /* hold the loop object alive for the duration */
        if (loop != AUDIO_POOL_HANDLE_NONE) {
            if (audio_pool_ref(&svc->pool, loop) != AUDIO_POOL_OK) return false;
        }

        /* adapter: stream_id 0 = intro, 1 = loop */
        if (!audio_pool_stream_init(&ms->stream_ctx, &svc->pool,
                                    MIXER_SRC_PCM16_MONO)) {
            if (loop != AUDIO_POOL_HANDLE_NONE)
                audio_pool_unref(&svc->pool, loop, NULL);
            return false;
        }
        audio_pool_stream_bind(&ms->stream_ctx, 0, intro);
        if (loop != AUDIO_POOL_HANDLE_NONE)
            audio_pool_stream_bind(&ms->stream_ctx, 1, loop);

        if (p) {
            mixer_set_volume(svc->mixer, track, (q15_t)p->gain);
            mixer_set_pan(svc->mixer, track, (q15_t)p->pan);
        }
        mixer_channel_reset(svc->mixer, track);

        MusicPlayerConfig mpc = {
            .mixer            = svc->mixer,
            .mixer_channel    = track,
            .format           = MIXER_SRC_PCM16_MONO,
            .stream_fn        = audio_pool_stream_read,
            .stream_user_data = &ms->stream_ctx,
            .intro_stream_id  = 0,
            .loop_stream_id   = (loop != AUDIO_POOL_HANDLE_NONE) ? 1
                                                                 : MUSIC_STREAM_NONE,
            .streaming_buffer = ms->streaming_buf,
            .streaming_buffer_samples = MUSIC_STREAM_FRAMES,
            .intro_head_buffer = ms->intro_head,
            .intro_head_samples = MUSIC_HEAD_FRAMES,
            .loop_head_buffer = ms->loop_head,
            .loop_head_samples = MUSIC_HEAD_FRAMES,
        };
        ms->player = music_create(&mpc);
        if (!ms->player) {
            if (loop != AUDIO_POOL_HANDLE_NONE)
                audio_pool_unref(&svc->pool, loop, NULL);
            return false;
        }
        ms->track  = track;
        ms->in_use = true;

        music_prime_intro(ms->player);
        if (loop != AUDIO_POOL_HANDLE_NONE) music_prime_loop(ms->player);
        music_play(ms->player);
        return true;
    }

    /* SFX: feed the whole sample into the mixer channel. The object
     * may span multiple blocks; copy in scratch-sized chunks. */
    uint32_t size = audio_pool_object_size(&svc->pool, object);
    if (size == 0) return false;

    /* per-voice gain / pan */
    if (p) {
        mixer_set_volume(svc->mixer, track, (q15_t)p->gain);
        mixer_set_pan(svc->mixer, track, (q15_t)p->pan);
    }
    mixer_channel_reset(svc->mixer, track);

    uint32_t off = 0;
    while (off < size) {
        uint32_t want = size - off;
        if (want > sizeof(svc->scratch)) want = sizeof(svc->scratch);
        uint32_t got = 0;
        if (audio_pool_read(&svc->pool, object, off, svc->scratch, want, &got)
                != AUDIO_POOL_OK || got == 0) {
            break;
        }
        /* mixer_write_channel counts in samples/frames, not bytes.
         * The mixer channel for SFX is configured PCM16_MONO in this
         * service, so 2 bytes/sample. (A fuller version would track
         * per-object format.) */
        size_t samples = got / 2u;
        mixer_write_channel(svc->mixer, track, svc->scratch, samples);
        off += got;
    }

    mixer_channel_start(svc->mixer, track);
    return true;
}

static void svc_sink_stop(void *ctx, uint32_t track) {
    AudioService *svc = (AudioService *)ctx;

    /* If a music player drives this track, tear it down and release
     * its loop ref. (The arbiter releases the intro/object ref.) */
    MusicSlot *ms = music_slot_for_track(svc, track);
    if (ms) {
        if (ms->player) { music_destroy(ms->player); ms->player = NULL; }
        AudioObjHandle loop = ms->stream_ctx.handle[1];
        if (loop != AUDIO_POOL_HANDLE_NONE)
            audio_pool_unref(&svc->pool, loop, NULL);
        ms->in_use = false;
    }

    mixer_channel_stop(svc->mixer, track);
    mixer_channel_reset(svc->mixer, track);
}

/* ---- create / destroy ---- */

AudioService *audio_service_create(const AudioServiceConfig *cfg) {
    if (!cfg || !cfg->channel || !cfg->pool_region) return NULL;
    uint32_t tracks = cfg->track_count ? cfg->track_count : 16u;
    if (tracks > AUDIO_ARBITER_MAX_TRACKS) tracks = AUDIO_ARBITER_MAX_TRACKS;

    AudioService *svc = calloc(1, sizeof(*svc));
    if (!svc) return NULL;
    svc->channel     = cfg->channel;
    svc->sample_rate = cfg->sample_rate ? cfg->sample_rate : 44100u;
    svc->track_count = tracks;
    svc->staging     = (uint8_t *)cfg->staging_buffer;
    svc->staging_cap = cfg->staging_capacity;

    if (audio_pool_init(&svc->pool, cfg->pool_region, cfg->pool_region_size)
            != AUDIO_POOL_OK) {
        free(svc);
        return NULL;
    }

    /* Build a mixer with `tracks` channels, all PCM16_MONO for SFX,
     * stereo q15 output. */
    MixerChannelConfig *chans = calloc(tracks, sizeof(MixerChannelConfig));
    if (!chans) { audio_pool_destroy(&svc->pool); free(svc); return NULL; }
    for (uint32_t i = 0; i < tracks; i++) {
        chans[i].format         = MIXER_SRC_PCM16_MONO;
        chans[i].buffer_samples = 4096;          /* per-channel ring */
        chans[i].volume         = Q15_ONE;
    }
    MixerOutputFormat out = { .bits = 16, .is_signed = true,
                              .storage_bits = 16, .channels = 2 };
    svc->mixer = mixer_create(chans, tracks, svc->sample_rate, out, 0);
    free(chans);
    if (!svc->mixer) { audio_pool_destroy(&svc->pool); free(svc); return NULL; }

    AudioArbiterSink sink = { svc_sink_start, svc_sink_stop, svc };
    if (!audio_arbiter_init(&svc->arbiter, tracks, &svc->pool, &sink)) {
        mixer_destroy(svc->mixer);
        audio_pool_destroy(&svc->pool);
        free(svc);
        return NULL;
    }

    /* Music object table generations start at 1. */
    for (int i = 0; i < MUSIC_MAX_OBJECTS; i++) svc->music_gen[i] = 1;

    /* FFT band meter (starts disabled — zero cost until enabled). */
    audio_fft_init(&svc->fft, svc->sample_rate);

    /* Allocate per-music-stream buffers (contiguous SRAM staging — NOT
     * block pool). One streaming buffer + two pinned heads per slot. */
    for (int i = 0; i < AUDIO_SERVICE_MAX_MUSIC; i++) {
        MusicSlot *ms = &svc->music_slots[i];
        ms->streaming_buf = malloc(MUSIC_STREAM_FRAMES * sizeof(int16_t));
        ms->intro_head    = malloc(MUSIC_HEAD_FRAMES   * sizeof(int16_t));
        ms->loop_head     = malloc(MUSIC_HEAD_FRAMES   * sizeof(int16_t));
        if (!ms->streaming_buf || !ms->intro_head || !ms->loop_head) {
            /* roll back everything */
            for (int j = 0; j <= i; j++) {
                free(svc->music_slots[j].streaming_buf);
                free(svc->music_slots[j].intro_head);
                free(svc->music_slots[j].loop_head);
            }
            mixer_destroy(svc->mixer);
            audio_pool_destroy(&svc->pool);
            free(svc);
            return NULL;
        }
    }
    return svc;
}

void audio_service_destroy(AudioService *svc) {
    if (!svc) return;
    /* tear down any live music players + their buffers */
    for (int i = 0; i < AUDIO_SERVICE_MAX_MUSIC; i++) {
        MusicSlot *ms = &svc->music_slots[i];
        if (ms->player) { music_destroy(ms->player); ms->player = NULL; }
        free(ms->streaming_buf);
        free(ms->intro_head);
        free(ms->loop_head);
    }
    if (svc->mixer) mixer_destroy(svc->mixer);
    audio_pool_destroy(&svc->pool);
    free(svc);
}

/* ---- request handling ---- */

/* Build + post a response echoing seq, with a3=status, a4=handle. */
static void respond(AudioService *svc, const ChannelMsg *req,
                    uint32_t status, uint32_t ret_handle) {
    if (!(req->flags & CHANNEL_FLAG_EXPECTS_RESPONSE)) return;
    ChannelMsg resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = req->type;
    resp.seq  = req->seq;
    resp.a3   = status;
    resp.a4   = ret_handle;
    /* retry until the response ring accepts it */
    while (!channel_response_post(svc->channel, &resp)) {
        channel_provider_wait(svc->channel, 1);
    }
}

static void handle_one(AudioService *svc, const ChannelMsg *m) {
    switch (m->type) {
    case REQ_AUDIO_POOL_ALLOC: {
        /* a0 = size, a1 = owner_vm */
        AudioObjHandle h;
        AudioPoolResult r = audio_pool_alloc(&svc->pool, m->a0,
                                             (uint16_t)m->a1, &h);
        respond(svc, m, (uint32_t)r, (r == AUDIO_POOL_OK) ? h : 0);
        break;
    }
    case REQ_AUDIO_POOL_FREE: {
        /* a0 = object handle (sample) OR a tagged music handle. */
        AudioObjHandle h = m->a0;
        if (is_music_handle(h)) {
            MusicObject *mo = resolve_music(svc, h);
            if (!mo) { respond(svc, m, (uint32_t)AUDIO_POOL_ERR_BAD_HANDLE, 0); break; }
            uint32_t slot = 0; uint16_t gen = 0; unpack_music(h, &slot, &gen);
            /* drop the music object's refs on its underlying pool objects */
            audio_pool_unref(&svc->pool, mo->intro, NULL);
            if (mo->loop != AUDIO_POOL_HANDLE_NONE)
                audio_pool_unref(&svc->pool, mo->loop, NULL);
            mo->in_use = false;
            /* bump gen so stale music handles are rejected */
            svc->music_gen[slot] = (uint16_t)((svc->music_gen[slot] + 1u) & 0x7FFFu);
            if (svc->music_gen[slot] == 0) svc->music_gen[slot] = 1;
            respond(svc, m, (uint32_t)AUDIO_POOL_OK, 1);
            break;
        }
        bool freed = false;
        AudioPoolResult r = audio_pool_unref(&svc->pool, h, &freed);
        respond(svc, m, (uint32_t)r, freed ? 1u : 0u);
        break;
    }
    case REQ_AUDIO_LOAD_SAMPLE: {
        /* a0 = size, a1 = owner_vm, a2 = shared-buffer handle/ptr of
         * the PCM to copy in. In this desktop cut the payload arrives
         * by a host-side pointer convention handled in the ecall
         * layer; the service just allocates and the ecall layer fills
         * it. So here LOAD == ALLOC; data copy is the caller's. */
        AudioObjHandle h;
        AudioPoolResult r = audio_pool_alloc(&svc->pool, m->a0,
                                             (uint16_t)m->a1, &h);
        respond(svc, m, (uint32_t)r, (r == AUDIO_POOL_OK) ? h : 0);
        break;
    }
    case REQ_AUDIO_TRIGGER_SFX: {
        /* a0 = object, a1 = gain(q15), a2 = pan(q15), a3 = owner_vm */
        AudioVoiceParams p = { .gain = (int32_t)m->a1, .pan = (int32_t)m->a2,
                               .priority = 0, .loop = 0 };
        AudioVoiceHandle v;
        AudioArbResult r = audio_arbiter_play(&svc->arbiter, m->a0,
                                              AUDIO_VOICE_SFX, &p,
                                              (uint16_t)m->a3, &v);
        respond(svc, m, (uint32_t)r, (r == AUDIO_ARB_OK) ? v : 0);
        break;
    }
    case REQ_AUDIO_LOAD_MUSIC: {
        /* a0 = intro pool object, a1 = loop pool object (or 0/NONE for
         * intro-only), a2 = owner_vm. Both must already be loaded
         * (via REQ_AUDIO_LOAD_STAGED). We take a ref on each so the
         * music object owns them, find a free music-table slot, and
         * return a tagged music handle. */
        AudioObjHandle intro = m->a0;
        AudioObjHandle loop  = m->a1;
        if (!audio_pool_handle_valid(&svc->pool, intro)) {
            respond(svc, m, (uint32_t)AUDIO_POOL_ERR_BAD_HANDLE, 0);
            break;
        }
        if (loop != AUDIO_POOL_HANDLE_NONE &&
            !audio_pool_handle_valid(&svc->pool, loop)) {
            respond(svc, m, (uint32_t)AUDIO_POOL_ERR_BAD_HANDLE, 0);
            break;
        }
        int slot = -1;
        for (int i = 0; i < MUSIC_MAX_OBJECTS; i++)
            if (!svc->music_objs[i].in_use) { slot = i; break; }
        if (slot < 0) { respond(svc, m, (uint32_t)AUDIO_POOL_ERR_NO_OBJECTS, 0); break; }

        /* the music object holds a ref on each underlying pool object */
        audio_pool_ref(&svc->pool, intro);
        if (loop != AUDIO_POOL_HANDLE_NONE) audio_pool_ref(&svc->pool, loop);

        MusicObject *mo = &svc->music_objs[slot];
        mo->in_use   = true;
        mo->intro    = intro;
        mo->loop     = loop;
        mo->owner_vm = (uint16_t)m->a2;
        respond(svc, m, (uint32_t)AUDIO_POOL_OK,
                pack_music((uint32_t)slot, svc->music_gen[slot]));
        break;
    }
    case REQ_AUDIO_PLAY_MUSIC: {
        /* a0 = music handle (tagged), a3 = owner_vm. Resolve to its
         * intro/loop pair; stash loop in pend_loop; play the intro as
         * the arbiter's object with kind=MUSIC. */
        MusicObject *mo = resolve_music(svc, m->a0);
        if (!mo) { respond(svc, m, (uint32_t)AUDIO_ARB_BAD_OBJECT, 0); break; }
        svc->pend_intro = mo->intro;
        svc->pend_loop  = mo->loop;
        AudioVoiceParams p = { .gain = Q15_ONE, .pan = 0, .priority = 0, .loop = 0 };
        AudioVoiceHandle v;
        AudioArbResult r = audio_arbiter_play(&svc->arbiter, mo->intro,
                                              AUDIO_VOICE_MUSIC, &p,
                                              (uint16_t)m->a3, &v);
        svc->pend_loop = AUDIO_POOL_HANDLE_NONE;
        svc->pend_intro = AUDIO_POOL_HANDLE_NONE;
        respond(svc, m, (uint32_t)r, (r == AUDIO_ARB_OK) ? v : 0);
        break;
    }
    case REQ_AUDIO_LOAD_STAGED: {
        /* a0 = byte size, a1 = owner_vm. PCM is already in the shared
         * staging buffer (the requester put it there). Alloc a pool
         * object and copy staging->pool, service-side (no race). */
        uint32_t size = m->a0;
        if (!svc->staging || size == 0 || size > svc->staging_cap) {
            respond(svc, m, (uint32_t)AUDIO_POOL_ERR_INVALID_ARG, 0);
            break;
        }
        AudioObjHandle h;
        AudioPoolResult r = audio_pool_alloc(&svc->pool, size,
                                             (uint16_t)m->a1, &h);
        if (r != AUDIO_POOL_OK) { respond(svc, m, (uint32_t)r, 0); break; }
        uint32_t wrote = 0;
        audio_pool_write(&svc->pool, h, 0, svc->staging, size, &wrote);
        respond(svc, m, (uint32_t)AUDIO_POOL_OK, h);
        break;
    }
    case REQ_AUDIO_SET_GAIN: {
        /* a0 = voice handle, a1 = gain q15. Map voice -> track, set
         * the mixer channel volume. */
        AudioObjHandle dummy = audio_arbiter_voice_object(&svc->arbiter, m->a0);
        if (dummy == AUDIO_POOL_HANDLE_NONE) {
            respond(svc, m, (uint32_t)AUDIO_ARB_BAD_VOICE, 0);
            break;
        }
        /* voice handle low 16 bits = track+1 (see arbiter packing) */
        uint32_t track = (m->a0 & 0xFFFFu) - 1u;
        mixer_set_volume(svc->mixer, track, (q15_t)m->a1);
        respond(svc, m, (uint32_t)AUDIO_ARB_OK, 0);
        break;
    }
    case REQ_AUDIO_FFT_ENABLE: {
        /* a0 = 1 enable, 0 disable */
        audio_fft_set_enabled(&svc->fft, m->a0 != 0);
        respond(svc, m, (uint32_t)AUDIO_ARB_OK, 0);
        break;
    }
    case REQ_AUDIO_GET_LEVELS: {
        /* Pack up to 16 band bytes into the response a0..a3 (4 bytes
         * each), a4 = band count. */
        if (!(m->flags & CHANNEL_FLAG_EXPECTS_RESPONSE)) break;
        uint8_t bands[AUDIO_FFT_BANDS];
        uint32_t got = audio_fft_get_bands(&svc->fft, bands,
                                           AUDIO_FFT_BANDS);
        ChannelMsg resp;
        memset(&resp, 0, sizeof(resp));
        resp.type = m->type;
        resp.seq  = m->seq;
        uint32_t words[4] = {0,0,0,0};
        for (uint32_t i = 0; i < got && i < 16; i++)
            words[i >> 2] |= (uint32_t)bands[i] << ((i & 3) * 8);
        resp.a0 = words[0]; resp.a1 = words[1];
        resp.a2 = words[2]; resp.a3 = words[3];
        resp.a4 = got;
        while (!channel_response_post(svc->channel, &resp))
            channel_provider_wait(svc->channel, 1);
        break;
    }
    case REQ_AUDIO_VOICE_STOP:
    case REQ_AUDIO_STOP_MUSIC: {
        /* a0 = voice handle */
        AudioArbResult r = audio_arbiter_stop(&svc->arbiter, m->a0);
        respond(svc, m, (uint32_t)r, 0);
        break;
    }
    default:
        respond(svc, m, (uint32_t)AUDIO_ARB_INVALID_ARG, 0);
        break;
    }
}

/* Pump all active music players (non-RT: refills their streaming
 * buffers from the pool). Called from the service loop, NOT from
 * render. */
static void pump_music(AudioService *svc) {
    for (int i = 0; i < AUDIO_SERVICE_MAX_MUSIC; i++) {
        MusicSlot *ms = &svc->music_slots[i];
        if (ms->in_use && ms->player) music_update(ms->player);
    }
}

uint32_t audio_service_process(AudioService *svc, uint32_t max) {
    if (!svc) return 0;
    uint32_t n = 0;
    ChannelMsg m;
    while (n < max && channel_request_poll(svc->channel, &m)) {
        handle_one(svc, &m);
        n++;
    }
    /* Keep music streaming buffers fed (non-RT). */
    pump_music(svc);
    /* Refresh the band meter (non-RT; no-op unless enabled + a fresh
     * window has accumulated). The FFT runs HERE, not in render. */
    audio_fft_update(&svc->fft);
    return n;
}

void audio_service_render(AudioService *svc, int16_t *out, uint32_t frames) {
    if (!svc || !out) return;
    mixer_render(svc->mixer, out, frames);
    /* RT-safe: append the mixed output to the FFT capture window.
     * No-op when meters are disabled. The FFT itself runs in the
     * non-RT process loop (audio_fft_update), never here. */
    audio_fft_capture(&svc->fft, out, frames);
}

void audio_service_run(AudioService *svc,
                       audio_service_stop_fn should_stop, void *user) {
    if (!svc) return;
    while (!should_stop(user)) {
        uint32_t did = audio_service_process(svc, 64);
        if (did == 0) {
            /* idle: block until a request may arrive */
            channel_provider_wait(svc->channel, 5);
        }
        /* NOTE: a real deployment fills the output ring on the audio
         * clock here (or in a separate render callback driven by the
         * DAC DMA). This loop is the request-processing half. */
    }
}

/* ---- introspection ---- */

void audio_service_sweep_vm(AudioService *svc, uint16_t vm_id) {
    if (!svc) return;
    /* Stop the VM's voices first (drops their pool refs), then drop
     * the VM's object-creation refs. */
    audio_arbiter_sweep_vm(&svc->arbiter, vm_id, NULL);
    audio_pool_sweep_vm(&svc->pool, vm_id, NULL);
}

AudioPool    *audio_service_pool(AudioService *svc)    { return svc ? &svc->pool : NULL; }
AudioArbiter *audio_service_arbiter(AudioService *svc) { return svc ? &svc->arbiter : NULL; }
AudioMixer   *audio_service_mixer(AudioService *svc)   { return svc ? svc->mixer : NULL; }
