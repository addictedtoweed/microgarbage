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

#include <stdlib.h>
#include <string.h>

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

    /* Scratch buffer for moving PCM from the pool into a mixer
     * channel on SFX start. Sized to one block. */
    uint8_t         scratch[AUDIO_POOL_BLOCK_SIZE];
};

/* ---- arbiter sink: start/stop drive the mixer ---- */

/* The sink ctx is the service itself. On SFX start we pull the
 * object's PCM from the pool and feed it into the track's mixer
 * channel, then start the channel. */
static bool svc_sink_start(void *ctx, uint32_t track, AudioVoiceKind kind,
                           AudioObjHandle object, const AudioVoiceParams *p) {
    AudioService *svc = (AudioService *)ctx;

    if (kind == AUDIO_VOICE_MUSIC) {
        /* MUSIC PATH — not yet wired (needs a music_player instance
         * bound to this track via the pool adapter). Decline for now
         * so play() returns REJECTED rather than silently lying. */
        return false;
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
    return svc;
}

void audio_service_destroy(AudioService *svc) {
    if (!svc) return;
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
        /* a0 = object handle */
        bool freed = false;
        AudioPoolResult r = audio_pool_unref(&svc->pool, m->a0, &freed);
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
    case REQ_AUDIO_PLAY_MUSIC: {
        /* a0 = music object, a3 = owner_vm. (Music path declines in
         * the sink for now -> REJECTED.) */
        AudioVoiceParams p = { .gain = Q15_ONE, .pan = 0, .priority = 0, .loop = 0 };
        AudioVoiceHandle v;
        AudioArbResult r = audio_arbiter_play(&svc->arbiter, m->a0,
                                              AUDIO_VOICE_MUSIC, &p,
                                              (uint16_t)m->a3, &v);
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

uint32_t audio_service_process(AudioService *svc, uint32_t max) {
    if (!svc) return 0;
    uint32_t n = 0;
    ChannelMsg m;
    while (n < max && channel_request_poll(svc->channel, &m)) {
        handle_one(svc, &m);
        n++;
    }
    return n;
}

void audio_service_render(AudioService *svc, int16_t *out, uint32_t frames) {
    if (!svc || !out) return;
    mixer_render(svc->mixer, out, frames);
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
