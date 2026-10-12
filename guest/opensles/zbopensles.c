/* Guest libOpenSLES.so: OpenSL ES 1.0.1 + Android extensions.
 *
 * Guests link libOpenSLES.so by DT_NEEDED, and one missing library stops the whole guest
 * library from loading. OpenSL ES is an object/interface API (vtables reached through
 * SLObjectItf), not a flat function list, so the generated svc trap stubs cannot serve it.
 * The objects live here, in guest code; the host only plays (core/include/zb/audio_protocol.h):
 *   - engine, output mix and audio player objects; everything else is FEATURE_UNSUPPORTED;
 *   - a PCM buffer queue player gets its own output thread, which hands each buffer to a host
 *     AAudio stream and calls the buffer queue callback once the device took it;
 *   - a URI / Android fd player is a player of the host's own OpenSL ES, which decodes it; the
 *     worker polls it for prefetch, errors and HEADATEND;
 *   - Android decode-to-buffer-queue players are refused (CONTENT_UNSUPPORTED), since no PCM
 *     would ever arrive.
 * Without host audio (zbrun, or a format the host cannot play) a player stays silent: buffer
 * queues are consumed in real time by the worker, and media players report SUFFICIENTDATA, an
 * unknown duration and a wall-clock position.
 *
 * Every API call takes one global lock. Callbacks run on the worker or output threads without
 * it, so they may call back in (Enqueue from the buffer queue callback is the usual pattern). */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "zb/audio_protocol.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#define ZB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "zbopensles", __VA_ARGS__)
#else
#include <stdio.h>
#define ZB_LOG(...) (fprintf(stderr, "zbopensles: " __VA_ARGS__), fputc('\n', stderr))
#endif

#ifndef SL_ANDROID_DATAFORMAT_PCM_EX
#define SL_ANDROID_DATAFORMAT_PCM_EX ((SLuint32)0x4)
#endif

#ifndef SL_ANDROID_PCM_REPRESENTATION_FLOAT
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT ((SLuint32)0x3)
#endif

#define ZB_EXPORT __attribute__((visibility("default")))

/* Host calls (hostcalls.S: svc #0x5AFAxx, arguments and results in r0-r2 as AAPCS places them). */
uint32_t zbsl_hc_probe(void);
uint32_t zbsl_hc_pcm_open(uint32_t sample_rate, uint32_t channels, uint32_t encoding);
uint32_t zbsl_hc_pcm_write(uint32_t handle, const void *data, uint32_t bytes);
int32_t zbsl_hc_pcm_control(uint32_t handle, uint32_t op, uint32_t argument);
uint32_t zbsl_hc_pcm_close(uint32_t handle);
uint32_t zbsl_hc_media_open(const struct zb_audio_media *media);
int32_t zbsl_hc_media_control(uint32_t handle, uint32_t op, uint32_t argument);
uint32_t zbsl_hc_media_query(uint32_t handle, uint32_t what);
uint32_t zbsl_hc_media_close(uint32_t handle);

/* ---- interface IDs ------------------------------------------------------------------- */
/* GUID values match Android's libOpenSLES (except where noted), so a guest that carries its
 * own copy of an IID still compares equal. */

#define ZB_GUID_TAIL 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b
#define ZB_IID(name, a, b, c, d, ...) \
    static const struct SLInterfaceID_ zb_iid_##name = {a, b, c, d, {__VA_ARGS__}}; \
    ZB_EXPORT const SLInterfaceID SL_IID_##name = &zb_iid_##name;

ZB_IID(3DCOMMIT, 0x3564ad80, 0xdd0f, 0x11db, 0x9e19, ZB_GUID_TAIL)
ZB_IID(3DDOPPLER, 0xb45c9a80, 0xddd2, 0x11db, 0xb028, ZB_GUID_TAIL)
ZB_IID(3DGROUPING, 0xebe844e0, 0xddd2, 0x11db, 0xb510, ZB_GUID_TAIL)
ZB_IID(3DLOCATION, 0x2b878020, 0xddd3, 0x11db, 0x8a01, ZB_GUID_TAIL)
ZB_IID(3DMACROSCOPIC, 0x5089aec0, 0xddd3, 0x11db, 0x9ad3, ZB_GUID_TAIL)
ZB_IID(3DSOURCE, 0x70bc7b00, 0xddd3, 0x11db, 0xa873, ZB_GUID_TAIL)
ZB_IID(AUDIODECODERCAPABILITIES, 0x3fe5a3a0, 0xfcc6, 0x11db, 0x94ac, ZB_GUID_TAIL)
ZB_IID(AUDIOENCODER, 0xd7d5af7a, 0x351c, 0x41a6, 0x94ec, 0x1a, 0xc9, 0x5c, 0x71, 0x82, 0x2c)
ZB_IID(AUDIOENCODERCAPABILITIES, 0x0f52a340, 0xfcd1, 0x11db, 0xa993, ZB_GUID_TAIL)
ZB_IID(AUDIOIODEVICECAPABILITIES, 0xb2564dc0, 0xddd3, 0x11db, 0xbd62, ZB_GUID_TAIL)
ZB_IID(BASSBOOST, 0x0634f220, 0xddd4, 0x11db, 0xa0fc, ZB_GUID_TAIL)
ZB_IID(BUFFERQUEUE, 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, ZB_GUID_TAIL)
ZB_IID(DEVICEVOLUME, 0xe1634760, 0xf3e2, 0x11db, 0x9ca9, ZB_GUID_TAIL)
ZB_IID(DYNAMICINTERFACEMANAGEMENT, 0x63936540, 0xf775, 0x11db, 0x9cc4, ZB_GUID_TAIL)
ZB_IID(DYNAMICSOURCE, 0xc55cc100, 0x038b, 0x11dc, 0xbb45, ZB_GUID_TAIL)
ZB_IID(EFFECTSEND, 0x56e7d200, 0xddd4, 0x11db, 0xaefb, ZB_GUID_TAIL)
ZB_IID(ENGINE, 0x8d97c260, 0xddd4, 0x11db, 0x958f, ZB_GUID_TAIL)
ZB_IID(ENGINECAPABILITIES, 0x8320d0a0, 0xddd5, 0x11db, 0xa1b1, ZB_GUID_TAIL)
ZB_IID(ENVIRONMENTALREVERB, 0xc2e5d5f0, 0x94bd, 0x4763, 0x9cac, 0x4e, 0x23, 0x4d, 0x06, 0x83, 0x9e)
ZB_IID(EQUALIZER, 0x0bed4300, 0xddd6, 0x11db, 0x8f34, ZB_GUID_TAIL)
ZB_IID(LED, 0x2cc1cd80, 0xddd6, 0x11db, 0x807e, ZB_GUID_TAIL)
ZB_IID(METADATAEXTRACTION, 0xaa5b1f80, 0xddd6, 0x11db, 0xac8e, ZB_GUID_TAIL)
ZB_IID(METADATATRAVERSAL, 0xc43662c0, 0xddd6, 0x11db, 0xa7ab, ZB_GUID_TAIL)
ZB_IID(MIDIMESSAGE, 0xddf4a820, 0xddd6, 0x11db, 0xb174, ZB_GUID_TAIL)
ZB_IID(MIDIMUTESOLO, 0x039eaf80, 0xddd7, 0x11db, 0x9a02, ZB_GUID_TAIL)
ZB_IID(MIDITEMPO, 0x1f347400, 0xddd7, 0x11db, 0xa7ce, ZB_GUID_TAIL)
ZB_IID(MIDITIME, 0x3da51de0, 0xddd7, 0x11db, 0xaf70, ZB_GUID_TAIL)
ZB_IID(MUTESOLO, 0x5a28ebe0, 0xddd7, 0x11db, 0x8220, ZB_GUID_TAIL)
ZB_IID(NULL, 0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10)
ZB_IID(OBJECT, 0x79216360, 0xddd7, 0x11db, 0xac16, ZB_GUID_TAIL)
ZB_IID(OUTPUTMIX, 0x97750f60, 0xddd7, 0x11db, 0x92b1, ZB_GUID_TAIL)
ZB_IID(PITCH, 0xc7e8ee00, 0xddd7, 0x11db, 0xa42c, ZB_GUID_TAIL)
ZB_IID(PLAY, 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, ZB_GUID_TAIL)
ZB_IID(PLAYBACKRATE, 0x2e3b2a40, 0xddda, 0x11db, 0xa349, ZB_GUID_TAIL)
ZB_IID(PREFETCHSTATUS, 0x2a41ee80, 0xddd8, 0x11db, 0xa41f, ZB_GUID_TAIL)
ZB_IID(PRESETREVERB, 0x47382d60, 0xddd8, 0x11db, 0xbf3a, ZB_GUID_TAIL)
ZB_IID(RATEPITCH, 0x61b62e60, 0xddda, 0x11db, 0x9eb8, ZB_GUID_TAIL)
ZB_IID(RECORD, 0xc5657aa0, 0xdddb, 0x11db, 0x82f7, ZB_GUID_TAIL)
ZB_IID(SEEK, 0xd43135a0, 0xdddc, 0x11db, 0xb458, ZB_GUID_TAIL)
ZB_IID(THREADSYNC, 0xf6ac6b40, 0xdddc, 0x11db, 0xa62e, ZB_GUID_TAIL)
ZB_IID(VIBRA, 0x169a8d60, 0xdddd, 0x11db, 0x923d, ZB_GUID_TAIL)
ZB_IID(VIRTUALIZER, 0x37cc2c00, 0xdddd, 0x11db, 0x8577, ZB_GUID_TAIL)
ZB_IID(VISUALIZATION, 0xe46b26a0, 0xdddd, 0x11db, 0x8afd, ZB_GUID_TAIL)
ZB_IID(VOLUME, 0x09e8ede0, 0xddde, 0x11db, 0xb4f6, ZB_GUID_TAIL)
ZB_IID(ANDROIDEFFECT, 0xae12da60, 0x99ac, 0x11df, 0xb456, ZB_GUID_TAIL)
ZB_IID(ANDROIDEFFECTCAPABILITIES, 0x6a4f6d60, 0xb5e6, 0x11df, 0xbb3b, ZB_GUID_TAIL)
ZB_IID(ANDROIDEFFECTSEND, 0x7be462c0, 0xbc43, 0x11df, 0x8670, ZB_GUID_TAIL)
ZB_IID(ANDROIDCONFIGURATION, 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, ZB_GUID_TAIL)
ZB_IID(ANDROIDSIMPLEBUFFERQUEUE, 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, ZB_GUID_TAIL)
ZB_IID(ANDROIDBUFFERQUEUESOURCE, 0x7fc1a460, 0xeec1, 0x11df, 0xa306, ZB_GUID_TAIL)
/* Recorder pre-processing interfaces from newer NDKs. Their platform GUIDs were not at hand,
 * so these values are our own; no object here exposes them (recorders are refused). */
ZB_IID(ANDROIDACOUSTICECHOCANCELLATION, 0x7a620001, 0x5a42, 0x4f53, 0x8001, 0x7a, 0x62, 0x73, 0x6c, 0x65, 0x73)
ZB_IID(ANDROIDAUTOMATICGAINCONTROL, 0x7a620002, 0x5a42, 0x4f53, 0x8002, 0x7a, 0x62, 0x73, 0x6c, 0x65, 0x73)
ZB_IID(ANDROIDNOISESUPPRESSION, 0x7a620003, 0x5a42, 0x4f53, 0x8003, 0x7a, 0x62, 0x73, 0x6c, 0x65, 0x73)

static int iid_is(SLInterfaceID iid, SLInterfaceID known) {
    return iid != NULL && (iid == known || memcmp(iid, known, sizeof(*known)) == 0);
}

/* ---- objects ------------------------------------------------------------------------- */

enum { KIND_ENGINE = 1, KIND_OUTPUTMIX, KIND_PLAYER };
enum { SOURCE_BUFFERQUEUE = 1, SOURCE_MEDIA };

typedef struct zb_obj zb_obj;

/* Every interface handle is the address of one of these; the vtable comes first, as the
 * SL*Itf double-pointer types require. */
typedef struct {
    const void *vtable;
    zb_obj *owner;
} zb_itf;

struct zb_obj {
    zb_itf object;
    int kind;
    SLuint32 state;
    slObjectCallback object_cb;
    void *object_cb_ctx;
    int realize_event; /* async Realize finished; the worker delivers the callback */
    SLint32 priority;
    SLboolean preemptable;
    int destroyed;
    int free_after_callback;
    zb_obj *next;

    /* KIND_ENGINE */
    zb_itf engine;
    /* KIND_OUTPUTMIX */
    zb_itf outputmix;

    /* KIND_PLAYER */
    int source;
    zb_itf play, bufferqueue, simplebufferqueue, volume, seek, prefetch, mutesolo, rate,
        effectsend, androideffectsend, config;
    SLuint32 play_state;
    slPlayCallback play_cb;
    void *play_cb_ctx;
    SLuint32 play_mask;
    SLmillisecond marker;
    int marker_set, marker_fired;
    SLmillisecond update_period;
    SLmillisecond next_newpos;
    int64_t position_us;   /* media position accumulated while playing */
    int64_t clock_start;   /* monotonic us when PLAYING began, media players only */
    /* buffer queue */
    uint32_t capacity, count, head;
    uint32_t play_index;
    uint32_t *sizes;
    const uint8_t **buffers;
    uint32_t bytes_per_second;
    int64_t front_deadline; /* monotonic us when the front buffer is consumed; 0 = not timed */
    int64_t front_remaining; /* paused or stopped with this much of the front buffer left */
    slBufferQueueCallback bq_cb;
    void *bq_cb_ctx;
    const void *bq_caller;
    /* prefetch */
    slPrefetchCallback prefetch_cb;
    void *prefetch_cb_ctx;
    SLuint32 prefetch_mask;
    int prefetch_event;
    int prefetched;
    /* plain settings that only round-trip */
    SLmillibel volume_level;
    SLboolean mute, stereo_enabled;
    SLpermille stereo_position, playback_rate;
    SLboolean loop_enabled;
    SLmillisecond loop_start, loop_end;
    SLuint32 channel_mute, channel_solo, channels;
    SLmillibel direct_level, send_level;
    SLboolean send_enabled;

    /* host output: a PCM stream (buffer queue) or a host media player (URI / fd) */
    uint32_t encoding;          /* ZB_AUDIO_ENCODING_*, 0 when the host cannot play the format */
    uint32_t sample_rate_hz;
    uint32_t pcm;               /* host stream handle, 0 = silent */
    uint32_t media;             /* host media handle, 0 = silent */
    struct zb_audio_media media_source;
    char *uri;                  /* copy of the URI locator string */
    uint32_t front_offset;      /* bytes of the front buffer the host already played */
    uint32_t queue_generation;  /* bumped by Clear and STOPPED: a write in flight is stale */
    pthread_t out_thread;
    pthread_cond_t out_wake;
    int out_running, closing, out_in_callback;
    int64_t next_poll;          /* media: next host status poll, monotonic us */
    int host_prefetched, host_error;
    SLmillisecond host_duration;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_callback_done = PTHREAD_COND_INITIALIZER;
static zb_obj *g_objects;
static zb_obj *g_in_callback;
static pthread_t g_worker;
static int g_worker_started;
static int g_host_audio = -1; /* -1 not probed yet, 0 silent, 1 host output available */

static zb_obj *owner_of(const void *self) {
    return self ? (*(zb_itf *const *)&self)->owner : NULL;
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void log_once(int *flag, const char *what) {
    if (!*flag) {
        *flag = 1;
        ZB_LOG("unsupported: %s", what);
    }
}

static void *worker_main(void *arg);

static void free_object(zb_obj *o) {
    if (o->kind == KIND_PLAYER) pthread_cond_destroy(&o->out_wake);
    free(o->sizes);
    free(o->buffers);
    free(o->uri);
    free(o);
}

/* Linear gain from the volume interface: level in millibels, mute silences. */
static float player_gain(const zb_obj *p) {
    if (p->mute) return 0.0f;
    if (p->volume_level <= SL_MILLIBEL_MIN) return 0.0f;
    return powf(10.0f, (float)p->volume_level / 2000.0f);
}

/* Caller holds g_lock. */
static void push_gain(zb_obj *p) {
    float gain = player_gain(p);
    uint32_t bits;
    memcpy(&bits, &gain, sizeof bits);
    if (p->pcm) zbsl_hc_pcm_control(p->pcm, ZB_AUDIO_OP_GAIN, bits);
    if (p->media) zbsl_hc_media_control(p->media, ZB_AUDIO_OP_GAIN, bits);
}

/* Caller holds g_lock. */
static int host_audio_available(void) {
    if (g_host_audio < 0) g_host_audio = zbsl_hc_probe() == ZB_AUDIO_PROBE_MAGIC;
    return g_host_audio;
}

static int64_t buffer_duration_us(const zb_obj *p, uint32_t size);

/* One per PCM buffer queue player with a host stream. The host write blocks while the device
 * plays, so this thread is the player's clock: a buffer is dequeued, and its callback called,
 * once the device took all of it. Pause, flush and close make a write return early; the part
 * already played is remembered in front_offset. */
static void *pcm_thread(void *arg) {
    zb_obj *p = arg;
    pthread_mutex_lock(&g_lock);
    while (!p->closing) {
        if (p->play_state != SL_PLAYSTATE_PLAYING || p->count == 0) {
            pthread_cond_wait(&p->out_wake, &g_lock);
            continue;
        }
        const uint32_t index = p->head;
        const uint8_t *buffer = p->buffers[index];
        const uint32_t size = p->sizes[index];
        const uint32_t offset = p->front_offset;
        const uint32_t generation = p->queue_generation;
        const uint32_t pcm = p->pcm;
        pthread_mutex_unlock(&g_lock);
        uint32_t written = zbsl_hc_pcm_write(pcm, buffer + offset, size - offset);
        pthread_mutex_lock(&g_lock);
        if (p->closing) break;
        if (generation != p->queue_generation) continue; /* cleared or stopped meanwhile */
        if (written > size - offset) written = size - offset;
        p->front_offset += written;
        if (p->front_offset < size) {
            /* Interrupted by a pause. A write that took nothing while still playing would spin. */
            if (written == 0 && p->play_state == SL_PLAYSTATE_PLAYING) {
                pthread_mutex_unlock(&g_lock);
                struct timespec ts = {0, 5000000};
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&g_lock);
            }
            continue;
        }
        p->front_offset = 0;
        p->position_us += buffer_duration_us(p, size);
        p->head = (p->head + 1) % p->capacity;
        p->count--;
        p->play_index++;
        if (p->bq_cb) {
            slBufferQueueCallback cb = p->bq_cb;
            void *ctx = p->bq_cb_ctx;
            const void *caller = p->bq_caller;
            p->out_in_callback = 1;
            pthread_mutex_unlock(&g_lock);
            cb((SLBufferQueueItf)caller, ctx);
            pthread_mutex_lock(&g_lock);
            p->out_in_callback = 0;
        }
    }
    /* The worker may be inside a callback of this player; it must finish before anyone frees. */
    while (g_in_callback == p) pthread_cond_wait(&g_callback_done, &g_lock);
    p->out_running = 0;
    const int free_self = p->free_after_callback;
    pthread_cond_broadcast(&g_callback_done);
    pthread_mutex_unlock(&g_lock);
    if (free_self) free_object(p);
    return NULL;
}

/* Opens the host side of a player at Realize. Caller holds g_lock. A player the host cannot
 * play stays silent rather than failing: the game keeps running either way. */
static void open_host_output(zb_obj *p) {
    if (p->kind != KIND_PLAYER || !host_audio_available()) return;
    if (p->source == SOURCE_BUFFERQUEUE) {
        if (p->encoding == 0) return;
        p->pcm = zbsl_hc_pcm_open(p->sample_rate_hz, p->channels, p->encoding);
        if (!p->pcm) return;
        p->out_running = 1;
        if (pthread_create(&p->out_thread, NULL, pcm_thread, p) != 0) {
            p->out_running = 0;
            zbsl_hc_pcm_close(p->pcm);
            p->pcm = 0;
            return;
        }
        pthread_detach(p->out_thread);
    } else {
        if (p->media_source.kind == 0) return;
        if (p->media_source.kind == ZB_AUDIO_MEDIA_URI) p->media_source.uri = (uint32_t)(uintptr_t)p->uri;
        p->media = zbsl_hc_media_open(&p->media_source);
        if (!p->media) return;
        p->host_duration = SL_TIME_UNKNOWN;
        if (p->loop_enabled) zbsl_hc_media_control(p->media, ZB_AUDIO_OP_LOOP, 1);
    }
    push_gain(p);
}

/* Caller holds g_lock. */
static SLresult ensure_worker(void) {
    if (g_worker_started) return SL_RESULT_SUCCESS;
    if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0) return SL_RESULT_RESOURCE_ERROR;
    g_worker_started = 1;
    return SL_RESULT_SUCCESS;
}

/* ---- worker -------------------------------------------------------------------------- */

static int64_t buffer_duration_us(const zb_obj *p, uint32_t size) {
    if (p->bytes_per_second == 0) return 0;
    return (int64_t)size * 1000000 / p->bytes_per_second;
}

static SLmillisecond player_position_ms(const zb_obj *p, int64_t now) {
    if (p->media) return zbsl_hc_media_query(p->media, ZB_AUDIO_QUERY_POSITION);
    int64_t us = p->position_us;
    if (p->source == SOURCE_MEDIA && p->play_state == SL_PLAYSTATE_PLAYING) us += now - p->clock_start;
    return (SLmillisecond)(us / 1000);
}

typedef struct {
    int type;
    zb_obj *obj;
} zb_event;

enum { EV_NONE, EV_REALIZE, EV_PREFETCH, EV_BUFFER, EV_MARKER, EV_NEWPOS, EV_HEADATEND };

/* How often the worker asks a host media player for prefetch, error and end of content. */
#define MEDIA_POLL_US 20000

/* Finds one due event and updates the state it consumes. Caller holds g_lock. Returns the
 * wait in microseconds until the next timed event (-1: nothing timed) when none is due. */
static zb_event next_event(int64_t now, int64_t *wait) {
    zb_event ev = {EV_NONE, NULL};
    *wait = -1;
    for (zb_obj *o = g_objects; o; o = o->next) {
        if (o->destroyed) continue;
        if (o->realize_event) {
            o->realize_event = 0;
            if (o->object_cb) return (zb_event){EV_REALIZE, o};
        }
        if (o->kind != KIND_PLAYER || o->state != SL_OBJECT_STATE_REALIZED) continue;
        if (o->media && now >= o->next_poll) {
            o->next_poll = now + MEDIA_POLL_US;
            const uint32_t status = zbsl_hc_media_query(o->media, ZB_AUDIO_QUERY_STATUS);
            if ((status & ZB_AUDIO_STATUS_ERROR) && !o->host_error) {
                /* Android reports content it cannot play as an underflow with nothing buffered. */
                o->host_error = 1;
                o->prefetch_event = 1;
            } else if ((status & ZB_AUDIO_STATUS_PREFETCHED) && !o->host_prefetched) {
                o->host_prefetched = 1;
                o->host_duration = zbsl_hc_media_query(o->media, ZB_AUDIO_QUERY_DURATION);
                o->prefetch_event = 1;
            }
            if (status & ZB_AUDIO_STATUS_ENDED) {
                /* The host player went to PAUSED at the end, as the specification says. */
                if (o->play_state == SL_PLAYSTATE_PLAYING) o->play_state = SL_PLAYSTATE_PAUSED;
                if (o->play_cb && (o->play_mask & SL_PLAYEVENT_HEADATEND)) return (zb_event){EV_HEADATEND, o};
            }
        }
        if (o->media && (o->play_state != SL_PLAYSTATE_STOPPED || !o->host_prefetched)) {
            int64_t w = o->next_poll - now;
            if (*wait < 0 || w < *wait) *wait = w < 0 ? 0 : w;
        }
        if (o->prefetch_event) {
            o->prefetch_event = 0;
            if (o->prefetch_cb && (o->prefetch_mask & (SL_PREFETCHEVENT_STATUSCHANGE |
                                                       SL_PREFETCHEVENT_FILLLEVELCHANGE)))
                return (zb_event){EV_PREFETCH, o};
        }
        if (o->play_state != SL_PLAYSTATE_PLAYING) continue;
        /* A buffer queue with a host stream is consumed by its own output thread. */
        if (o->source == SOURCE_BUFFERQUEUE && !o->pcm && o->count > 0 && o->front_deadline != 0) {
            if (now >= o->front_deadline) {
                int64_t done = buffer_duration_us(o, o->sizes[o->head]);
                o->position_us += done;
                o->head = (o->head + 1) % o->capacity;
                o->count--;
                o->play_index++;
                if (o->count > 0) {
                    /* Keep the cadence, but never try to catch up after a long stall. */
                    int64_t base = o->front_deadline < now - 100000 ? now : o->front_deadline;
                    o->front_deadline = base + buffer_duration_us(o, o->sizes[o->head]);
                } else {
                    o->front_deadline = 0;
                }
                if (o->bq_cb) return (zb_event){EV_BUFFER, o};
            } else {
                int64_t w = o->front_deadline - now;
                if (*wait < 0 || w < *wait) *wait = w;
            }
        }
        if (o->play_cb) {
            SLmillisecond pos = player_position_ms(o, now);
            if (o->marker_set && !o->marker_fired && pos >= o->marker) {
                o->marker_fired = 1;
                if (o->play_mask & SL_PLAYEVENT_HEADATMARKER) return (zb_event){EV_MARKER, o};
            }
            if ((o->play_mask & SL_PLAYEVENT_HEADATNEWPOS) && o->update_period > 0) {
                if (pos >= o->next_newpos) {
                    o->next_newpos = (pos / o->update_period + 1) * o->update_period;
                    return (zb_event){EV_NEWPOS, o};
                }
            }
            /* Positions are polled; 10 ms is fine for marker and period resolution. */
            if (*wait < 0 || *wait > 10000) *wait = 10000;
        }
    }
    return ev;
}

static void *worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        int64_t wait;
        int64_t now = now_us();
        zb_event ev = next_event(now, &wait);
        if (ev.type == EV_NONE) {
            if (wait < 0) {
                pthread_cond_wait(&g_wake, &g_lock);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                int64_t ns = (int64_t)ts.tv_nsec + (wait < 1000 ? 1000 : wait) * 1000;
                ts.tv_sec += ns / 1000000000;
                ts.tv_nsec = ns % 1000000000;
                pthread_cond_timedwait(&g_wake, &g_lock, &ts);
            }
            continue;
        }
        zb_obj *o = ev.obj;
        g_in_callback = o;
        switch (ev.type) {
        case EV_REALIZE: {
            slObjectCallback cb = o->object_cb;
            void *ctx = o->object_cb_ctx;
            pthread_mutex_unlock(&g_lock);
            cb((SLObjectItf)&o->object, ctx, SL_OBJECT_EVENT_ASYNC_TERMINATION, SL_RESULT_SUCCESS,
               SL_OBJECT_STATE_REALIZED, NULL);
            break;
        }
        case EV_PREFETCH: {
            slPrefetchCallback cb = o->prefetch_cb;
            void *ctx = o->prefetch_cb_ctx;
            SLuint32 mask = o->prefetch_mask & (SL_PREFETCHEVENT_STATUSCHANGE |
                                                SL_PREFETCHEVENT_FILLLEVELCHANGE);
            pthread_mutex_unlock(&g_lock);
            cb((SLPrefetchStatusItf)&o->prefetch, ctx, mask);
            break;
        }
        case EV_BUFFER: {
            slBufferQueueCallback cb = o->bq_cb;
            void *ctx = o->bq_cb_ctx;
            const void *caller = o->bq_caller;
            pthread_mutex_unlock(&g_lock);
            cb((SLBufferQueueItf)caller, ctx);
            break;
        }
        default: {
            slPlayCallback cb = o->play_cb;
            void *ctx = o->play_cb_ctx;
            SLuint32 event = ev.type == EV_MARKER   ? SL_PLAYEVENT_HEADATMARKER
                             : ev.type == EV_NEWPOS ? SL_PLAYEVENT_HEADATNEWPOS
                                                    : SL_PLAYEVENT_HEADATEND;
            pthread_mutex_unlock(&g_lock);
            cb((SLPlayItf)&o->play, ctx, event);
            break;
        }
        }
        pthread_mutex_lock(&g_lock);
        g_in_callback = NULL;
        /* With an output thread still running, that thread frees the player when it exits. */
        if (o->free_after_callback && !o->out_running) free_object(o);
        pthread_cond_broadcast(&g_callback_done);
    }
    return NULL;
}

/* ---- SLObjectItf --------------------------------------------------------------------- */

static SLresult object_get_interface_locked(zb_obj *o, SLInterfaceID iid, void *out);

static SLresult obj_Realize(SLObjectItf self, SLboolean async) {
    zb_obj *o = owner_of(self);
    if (!o) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    SLresult r = SL_RESULT_SUCCESS;
    if (o->state != SL_OBJECT_STATE_UNREALIZED) {
        r = SL_RESULT_PRECONDITIONS_VIOLATED;
    } else {
        o->state = SL_OBJECT_STATE_REALIZED;
        open_host_output(o);
        if (o->media) r = ensure_worker();
        if (async && r == SL_RESULT_SUCCESS) {
            o->realize_event = 1;
            r = ensure_worker();
            pthread_cond_signal(&g_wake);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static SLresult obj_Resume(SLObjectItf self, SLboolean async) {
    (void)async;
    zb_obj *o = owner_of(self);
    if (!o) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    SLresult r = o->state == SL_OBJECT_STATE_SUSPENDED ? SL_RESULT_SUCCESS : SL_RESULT_PRECONDITIONS_VIOLATED;
    if (r == SL_RESULT_SUCCESS) o->state = SL_OBJECT_STATE_REALIZED;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static SLresult obj_GetState(SLObjectItf self, SLuint32 *state) {
    zb_obj *o = owner_of(self);
    if (!o || !state) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    *state = o->state;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *out) {
    zb_obj *o = owner_of(self);
    if (!o || !iid || !out) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    SLresult r = o->state == SL_OBJECT_STATE_REALIZED ? object_get_interface_locked(o, iid, out)
                                                      : SL_RESULT_PRECONDITIONS_VIOLATED;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static SLresult obj_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *ctx) {
    zb_obj *o = owner_of(self);
    if (!o) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    o->object_cb = cb;
    o->object_cb_ctx = ctx;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static void obj_AbortAsyncOperation(SLObjectItf self) { (void)self; }

static void obj_Destroy(SLObjectItf self) {
    zb_obj *o = owner_of(self);
    if (!o) return;
    pthread_mutex_lock(&g_lock);
    o->destroyed = 1;
    for (zb_obj **link = &g_objects; *link; link = &(*link)->next) {
        if (*link == o) {
            *link = o->next;
            break;
        }
    }
    if (o->media) {
        zbsl_hc_media_close(o->media);
        o->media = 0;
    }
    if (o->pcm) {
        /* The output thread leaves its loop; closing the stream ends a write in flight. */
        o->closing = 1;
        pthread_cond_signal(&o->out_wake);
        zbsl_hc_pcm_close(o->pcm);
        o->pcm = 0;
    }
    const pthread_t current = pthread_self();
    if ((g_in_callback == o && g_worker_started && pthread_equal(current, g_worker)) ||
        (o->out_running && o->out_in_callback && pthread_equal(current, o->out_thread))) {
        /* Destroyed from inside its own callback: whichever thread finishes last frees it. */
        o->free_after_callback = 1;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* Like the platform, Destroy returns only once no callback of the object is running. */
    while (g_in_callback == o || o->out_running) pthread_cond_wait(&g_callback_done, &g_lock);
    pthread_mutex_unlock(&g_lock);
    free_object(o);
}

static SLresult obj_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable) {
    zb_obj *o = owner_of(self);
    if (!o) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    o->priority = priority;
    o->preemptable = preemptable;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult obj_GetPriority(SLObjectItf self, SLint32 *priority, SLboolean *preemptable) {
    zb_obj *o = owner_of(self);
    if (!o || !priority || !preemptable) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    *priority = o->priority;
    *preemptable = o->preemptable;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult obj_SetLossOfControlInterfaces(SLObjectItf self, SLint16 n, SLInterfaceID *ids,
                                               SLboolean enabled) {
    (void)self, (void)n, (void)ids, (void)enabled;
    return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_vtable = {
    .Realize = obj_Realize,
    .Resume = obj_Resume,
    .GetState = obj_GetState,
    .GetInterface = obj_GetInterface,
    .RegisterCallback = obj_RegisterCallback,
    .AbortAsyncOperation = obj_AbortAsyncOperation,
    .Destroy = obj_Destroy,
    .SetPriority = obj_SetPriority,
    .GetPriority = obj_GetPriority,
    .SetLossOfControlInterfaces = obj_SetLossOfControlInterfaces,
};

/* ---- SLPlayItf ----------------------------------------------------------------------- */

#define LOCKED_PLAYER(self)                         \
    zb_obj *p = owner_of(self);                     \
    if (!p) return SL_RESULT_PARAMETER_INVALID;     \
    pthread_mutex_lock(&g_lock)
#define UNLOCK_RETURN(value)        \
    do {                            \
        pthread_mutex_unlock(&g_lock); \
        return (value);             \
    } while (0)

static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state) {
    if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    int64_t now = now_us();
    SLuint32 old = p->play_state;
    if (old == state) UNLOCK_RETURN(SL_RESULT_SUCCESS);
    if (old == SL_PLAYSTATE_PLAYING) {
        if (p->source == SOURCE_MEDIA) p->position_us += now - p->clock_start;
        if (p->front_deadline != 0) {
            p->front_remaining = p->front_deadline > now ? p->front_deadline - now : 0;
            p->front_deadline = 0;
        }
    }
    if (state == SL_PLAYSTATE_STOPPED) {
        p->position_us = 0;
        p->front_remaining = 0;
        p->marker_fired = 0;
        p->next_newpos = 0;
    }
    if (state == SL_PLAYSTATE_PLAYING) {
        p->clock_start = now;
        if (p->source == SOURCE_BUFFERQUEUE && p->count > 0) {
            int64_t left = p->front_remaining > 0 ? p->front_remaining
                                                  : buffer_duration_us(p, p->sizes[p->head]);
            p->front_deadline = now + left;
            p->front_remaining = 0;
        }
    }
    if (state != SL_PLAYSTATE_STOPPED && p->source == SOURCE_MEDIA && !p->media && !p->prefetched) {
        p->prefetched = 1;
        p->prefetch_event = 1;
    }
    p->play_state = state;
    if (p->pcm) {
        if (state == SL_PLAYSTATE_PLAYING) {
            zbsl_hc_pcm_control(p->pcm, ZB_AUDIO_OP_START, 0);
        } else if (state == SL_PLAYSTATE_PAUSED) {
            zbsl_hc_pcm_control(p->pcm, ZB_AUDIO_OP_PAUSE, 0);
        } else {
            /* Stopping keeps the queue but restarts its front buffer. */
            zbsl_hc_pcm_control(p->pcm, ZB_AUDIO_OP_FLUSH, 0);
            p->queue_generation++;
            p->front_offset = 0;
        }
        pthread_cond_signal(&p->out_wake);
    }
    if (p->media) {
        zbsl_hc_media_control(p->media, ZB_AUDIO_OP_PLAY_STATE, state);
        p->next_poll = 0;
    }
    SLresult r = ensure_worker();
    pthread_cond_signal(&g_wake);
    UNLOCK_RETURN(r);
}

static SLresult play_GetPlayState(SLPlayItf self, SLuint32 *state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *state = p->play_state;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_GetDuration(SLPlayItf self, SLmillisecond *msec) {
    if (!msec) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *msec = SL_TIME_UNKNOWN;
    if (p->media) {
        if (p->host_duration == SL_TIME_UNKNOWN)
            p->host_duration = zbsl_hc_media_query(p->media, ZB_AUDIO_QUERY_DURATION);
        *msec = p->host_duration;
    }
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_GetPosition(SLPlayItf self, SLmillisecond *msec) {
    if (!msec) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *msec = player_position_ms(p, now_us());
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_RegisterCallback(SLPlayItf self, slPlayCallback cb, void *ctx) {
    LOCKED_PLAYER(self);
    p->play_cb = cb;
    p->play_cb_ctx = ctx;
    pthread_cond_signal(&g_wake);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_SetCallbackEventsMask(SLPlayItf self, SLuint32 flags) {
    LOCKED_PLAYER(self);
    p->play_mask = flags;
    pthread_cond_signal(&g_wake);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *flags) {
    if (!flags) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *flags = p->play_mask;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_SetMarkerPosition(SLPlayItf self, SLmillisecond msec) {
    LOCKED_PLAYER(self);
    p->marker = msec;
    p->marker_set = 1;
    p->marker_fired = player_position_ms(p, now_us()) > msec;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_ClearMarkerPosition(SLPlayItf self) {
    LOCKED_PLAYER(self);
    p->marker_set = 0;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_GetMarkerPosition(SLPlayItf self, SLmillisecond *msec) {
    if (!msec) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    if (!p->marker_set) UNLOCK_RETURN(SL_RESULT_PRECONDITIONS_VIOLATED);
    *msec = p->marker;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond msec) {
    if (msec == 0) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    p->update_period = msec;
    SLmillisecond pos = player_position_ms(p, now_us());
    p->next_newpos = (pos / msec + 1) * msec;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond *msec) {
    if (!msec) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *msec = p->update_period;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static const struct SLPlayItf_ play_vtable = {
    .SetPlayState = play_SetPlayState,
    .GetPlayState = play_GetPlayState,
    .GetDuration = play_GetDuration,
    .GetPosition = play_GetPosition,
    .RegisterCallback = play_RegisterCallback,
    .SetCallbackEventsMask = play_SetCallbackEventsMask,
    .GetCallbackEventsMask = play_GetCallbackEventsMask,
    .SetMarkerPosition = play_SetMarkerPosition,
    .ClearMarkerPosition = play_ClearMarkerPosition,
    .GetMarkerPosition = play_GetMarkerPosition,
    .SetPositionUpdatePeriod = play_SetPositionUpdatePeriod,
    .GetPositionUpdatePeriod = play_GetPositionUpdatePeriod,
};

/* ---- buffer queues (SLBufferQueueItf and SLAndroidSimpleBufferQueueItf) --------------- */
/* Both interfaces have the same layout and share one queue; the callback receives the
 * interface it was registered on. */

static SLresult bq_enqueue(const void *self, const void *buffer, SLuint32 size) {
    if (!buffer || size == 0) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    if (p->count >= p->capacity) UNLOCK_RETURN(SL_RESULT_BUFFER_INSUFFICIENT);
    const uint32_t slot = (p->head + p->count) % p->capacity;
    p->sizes[slot] = size;
    p->buffers[slot] = buffer;
    p->count++;
    if (p->pcm) {
        pthread_cond_signal(&p->out_wake);
    } else if (p->count == 1 && p->play_state == SL_PLAYSTATE_PLAYING) {
        p->front_deadline = now_us() + buffer_duration_us(p, size);
        pthread_cond_signal(&g_wake);
    }
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult bq_clear(const void *self) {
    LOCKED_PLAYER(self);
    p->count = 0;
    p->front_deadline = 0;
    p->front_remaining = 0;
    if (p->pcm) {
        /* Drop what the device still holds; a write in flight returns and is discarded. */
        zbsl_hc_pcm_control(p->pcm, ZB_AUDIO_OP_FLUSH, 0);
        if (p->play_state == SL_PLAYSTATE_PLAYING) zbsl_hc_pcm_control(p->pcm, ZB_AUDIO_OP_START, 0);
        p->queue_generation++;
        p->front_offset = 0;
    }
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult bq_get_state(const void *self, SLuint32 *count, SLuint32 *index) {
    if (!count || !index) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *count = p->count;
    *index = p->play_index;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult bq_register(const void *self, slBufferQueueCallback cb, void *ctx) {
    LOCKED_PLAYER(self);
    p->bq_cb = cb;
    p->bq_cb_ctx = ctx;
    p->bq_caller = self;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult bq_Enqueue(SLBufferQueueItf self, const void *buffer, SLuint32 size) {
    return bq_enqueue(self, buffer, size);
}
static SLresult bq_Clear(SLBufferQueueItf self) { return bq_clear(self); }
static SLresult bq_GetState(SLBufferQueueItf self, SLBufferQueueState *state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    return bq_get_state(self, &state->count, &state->playIndex);
}
static SLresult bq_RegisterCallback(SLBufferQueueItf self, slBufferQueueCallback cb, void *ctx) {
    return bq_register(self, cb, ctx);
}

static const struct SLBufferQueueItf_ bufferqueue_vtable = {
    .Enqueue = bq_Enqueue,
    .Clear = bq_Clear,
    .GetState = bq_GetState,
    .RegisterCallback = bq_RegisterCallback,
};

static SLresult asbq_Enqueue(SLAndroidSimpleBufferQueueItf self, const void *buffer, SLuint32 size) {
    return bq_enqueue(self, buffer, size);
}
static SLresult asbq_Clear(SLAndroidSimpleBufferQueueItf self) { return bq_clear(self); }
static SLresult asbq_GetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState *state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    return bq_get_state(self, &state->count, &state->index);
}
static SLresult asbq_RegisterCallback(SLAndroidSimpleBufferQueueItf self,
                                      slAndroidSimpleBufferQueueCallback cb, void *ctx) {
    /* Same ABI as slBufferQueueCallback: (interface, context). */
    return bq_register(self, (slBufferQueueCallback)cb, ctx);
}

static const struct SLAndroidSimpleBufferQueueItf_ simplebufferqueue_vtable = {
    .Enqueue = asbq_Enqueue,
    .Clear = asbq_Clear,
    .GetState = asbq_GetState,
    .RegisterCallback = asbq_RegisterCallback,
};

/* ---- SLVolumeItf --------------------------------------------------------------------- */

static SLresult vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) {
    if (level > 0) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    p->volume_level = level;
    push_gain(p);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *level) {
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *level = p->volume_level;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *level) {
    (void)self;
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    *level = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_SetMute(SLVolumeItf self, SLboolean mute) {
    LOCKED_PLAYER(self);
    p->mute = mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    push_gain(p);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_GetMute(SLVolumeItf self, SLboolean *mute) {
    if (!mute) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *mute = p->mute;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_EnableStereoPosition(SLVolumeItf self, SLboolean enable) {
    LOCKED_PLAYER(self);
    p->stereo_enabled = enable ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean *enable) {
    if (!enable) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *enable = p->stereo_enabled;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_SetStereoPosition(SLVolumeItf self, SLpermille position) {
    if (position < -1000 || position > 1000) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    p->stereo_position = position;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult vol_GetStereoPosition(SLVolumeItf self, SLpermille *position) {
    if (!position) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *position = p->stereo_position;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static const struct SLVolumeItf_ volume_vtable = {
    .SetVolumeLevel = vol_SetVolumeLevel,
    .GetVolumeLevel = vol_GetVolumeLevel,
    .GetMaxVolumeLevel = vol_GetMaxVolumeLevel,
    .SetMute = vol_SetMute,
    .GetMute = vol_GetMute,
    .EnableStereoPosition = vol_EnableStereoPosition,
    .IsEnabledStereoPosition = vol_IsEnabledStereoPosition,
    .SetStereoPosition = vol_SetStereoPosition,
    .GetStereoPosition = vol_GetStereoPosition,
};

/* ---- SLSeekItf ----------------------------------------------------------------------- */

static SLresult seek_SetPosition(SLSeekItf self, SLmillisecond pos, SLuint32 mode) {
    (void)mode;
    LOCKED_PLAYER(self);
    int64_t now = now_us();
    p->position_us = (int64_t)pos * 1000;
    if (p->play_state == SL_PLAYSTATE_PLAYING) p->clock_start = now;
    p->marker_fired = p->marker_set && pos > p->marker;
    if (p->update_period) p->next_newpos = (pos / p->update_period + 1) * p->update_period;
    if (p->media) zbsl_hc_media_control(p->media, ZB_AUDIO_OP_SEEK, pos);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult seek_SetLoop(SLSeekItf self, SLboolean enable, SLmillisecond start, SLmillisecond end) {
    if (end != SL_TIME_UNKNOWN && end <= start) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    p->loop_enabled = enable ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    p->loop_start = start;
    p->loop_end = end;
    if (p->media) zbsl_hc_media_control(p->media, ZB_AUDIO_OP_LOOP, p->loop_enabled ? 1 : 0);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult seek_GetLoop(SLSeekItf self, SLboolean *enable, SLmillisecond *start, SLmillisecond *end) {
    if (!enable || !start || !end) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *enable = p->loop_enabled;
    *start = p->loop_start;
    *end = p->loop_end;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static const struct SLSeekItf_ seek_vtable = {
    .SetPosition = seek_SetPosition,
    .SetLoop = seek_SetLoop,
    .GetLoop = seek_GetLoop,
};

/* ---- SLPrefetchStatusItf ------------------------------------------------------------- */

/* A host media player reports what its decoder reached; a silent one always has everything. */
static SLresult pf_GetPrefetchStatus(SLPrefetchStatusItf self, SLuint32 *status) {
    if (!status) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *status = !p->media || (p->host_prefetched && !p->host_error) ? SL_PREFETCHSTATUS_SUFFICIENTDATA
                                                                  : SL_PREFETCHSTATUS_UNDERFLOW;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult pf_GetFillLevel(SLPrefetchStatusItf self, SLpermille *level) {
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *level = !p->media || (p->host_prefetched && !p->host_error) ? 1000 : 0;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult pf_RegisterCallback(SLPrefetchStatusItf self, slPrefetchCallback cb, void *ctx) {
    LOCKED_PLAYER(self);
    p->prefetch_cb = cb;
    p->prefetch_cb_ctx = ctx;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult pf_SetCallbackEventsMask(SLPrefetchStatusItf self, SLuint32 flags) {
    LOCKED_PLAYER(self);
    p->prefetch_mask = flags;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult pf_GetCallbackEventsMask(SLPrefetchStatusItf self, SLuint32 *flags) {
    if (!flags) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *flags = p->prefetch_mask;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult pf_SetFillUpdatePeriod(SLPrefetchStatusItf self, SLpermille period) {
    (void)self;
    return period > 0 && period <= 1000 ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID;
}
static SLresult pf_GetFillUpdatePeriod(SLPrefetchStatusItf self, SLpermille *period) {
    (void)self;
    if (!period) return SL_RESULT_PARAMETER_INVALID;
    *period = 100;
    return SL_RESULT_SUCCESS;
}

static const struct SLPrefetchStatusItf_ prefetch_vtable = {
    .GetPrefetchStatus = pf_GetPrefetchStatus,
    .GetFillLevel = pf_GetFillLevel,
    .RegisterCallback = pf_RegisterCallback,
    .SetCallbackEventsMask = pf_SetCallbackEventsMask,
    .GetCallbackEventsMask = pf_GetCallbackEventsMask,
    .SetFillUpdatePeriod = pf_SetFillUpdatePeriod,
    .GetFillUpdatePeriod = pf_GetFillUpdatePeriod,
};

/* ---- SLMuteSoloItf ------------------------------------------------------------------- */

static SLresult ms_set(const void *self, SLuint8 chan, SLboolean on, int solo) {
    LOCKED_PLAYER(self);
    if (chan >= p->channels) UNLOCK_RETURN(SL_RESULT_PARAMETER_INVALID);
    SLuint32 *bits = solo ? &p->channel_solo : &p->channel_mute;
    if (on) *bits |= 1u << chan;
    else *bits &= ~(1u << chan);
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult ms_get(const void *self, SLuint8 chan, SLboolean *on, int solo) {
    if (!on) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    if (chan >= p->channels) UNLOCK_RETURN(SL_RESULT_PARAMETER_INVALID);
    *on = ((solo ? p->channel_solo : p->channel_mute) >> chan) & 1 ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult ms_SetChannelMute(SLMuteSoloItf self, SLuint8 chan, SLboolean mute) { return ms_set(self, chan, mute, 0); }
static SLresult ms_GetChannelMute(SLMuteSoloItf self, SLuint8 chan, SLboolean *mute) { return ms_get(self, chan, mute, 0); }
static SLresult ms_SetChannelSolo(SLMuteSoloItf self, SLuint8 chan, SLboolean solo) { return ms_set(self, chan, solo, 1); }
static SLresult ms_GetChannelSolo(SLMuteSoloItf self, SLuint8 chan, SLboolean *solo) { return ms_get(self, chan, solo, 1); }
static SLresult ms_GetNumChannels(SLMuteSoloItf self, SLuint8 *n) {
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *n = (SLuint8)p->channels;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static const struct SLMuteSoloItf_ mutesolo_vtable = {
    .SetChannelMute = ms_SetChannelMute,
    .GetChannelMute = ms_GetChannelMute,
    .SetChannelSolo = ms_SetChannelSolo,
    .GetChannelSolo = ms_GetChannelSolo,
    .GetNumChannels = ms_GetNumChannels,
};

/* ---- SLPlaybackRateItf --------------------------------------------------------------- */

static SLresult rate_SetRate(SLPlaybackRateItf self, SLpermille rate) {
    if (rate < 500 || rate > 2000) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    p->playback_rate = rate;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult rate_GetRate(SLPlaybackRateItf self, SLpermille *rate) {
    if (!rate) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *rate = p->playback_rate;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult rate_SetPropertyConstraints(SLPlaybackRateItf self, SLuint32 constraints) {
    (void)self, (void)constraints;
    return SL_RESULT_SUCCESS;
}
static SLresult rate_GetProperties(SLPlaybackRateItf self, SLuint32 *properties) {
    (void)self;
    if (!properties) return SL_RESULT_PARAMETER_INVALID;
    *properties = SL_RATEPROP_NOPITCHCORAUDIO;
    return SL_RESULT_SUCCESS;
}
static SLresult rate_GetCapabilitiesOfRate(SLPlaybackRateItf self, SLpermille rate, SLuint32 *caps) {
    (void)self;
    if (!caps) return SL_RESULT_PARAMETER_INVALID;
    *caps = rate >= 500 && rate <= 2000 ? SL_RATEPROP_NOPITCHCORAUDIO : 0;
    return SL_RESULT_SUCCESS;
}
static SLresult rate_GetRateRange(SLPlaybackRateItf self, SLuint8 index, SLpermille *min, SLpermille *max,
                                  SLpermille *step, SLuint32 *caps) {
    (void)self;
    if (!min || !max || !step || !caps) return SL_RESULT_PARAMETER_INVALID;
    if (index != 0) return SL_RESULT_PARAMETER_INVALID;
    *min = 500;
    *max = 2000;
    *step = 0;
    *caps = SL_RATEPROP_NOPITCHCORAUDIO;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlaybackRateItf_ rate_vtable = {
    .SetRate = rate_SetRate,
    .GetRate = rate_GetRate,
    .SetPropertyConstraints = rate_SetPropertyConstraints,
    .GetProperties = rate_GetProperties,
    .GetCapabilitiesOfRate = rate_GetCapabilitiesOfRate,
    .GetRateRange = rate_GetRateRange,
};

/* ---- SLEffectSendItf / SLAndroidEffectSendItf ---------------------------------------- */
/* No effects exist, so sends only remember their levels. */

static SLresult es_enable(const void *self, SLboolean enable, SLmillibel level) {
    LOCKED_PLAYER(self);
    p->send_enabled = enable ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    p->send_level = level;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult es_is_enabled(const void *self, SLboolean *enable) {
    if (!enable) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *enable = p->send_enabled;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult es_set_level(const void *self, SLmillibel level, int direct) {
    LOCKED_PLAYER(self);
    if (direct) p->direct_level = level;
    else p->send_level = level;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}
static SLresult es_get_level(const void *self, SLmillibel *level, int direct) {
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    LOCKED_PLAYER(self);
    *level = direct ? p->direct_level : p->send_level;
    UNLOCK_RETURN(SL_RESULT_SUCCESS);
}

static SLresult es_EnableEffectSend(SLEffectSendItf self, const void *aux, SLboolean enable, SLmillibel level) {
    (void)aux;
    return es_enable(self, enable, level);
}
static SLresult es_IsEnabled(SLEffectSendItf self, const void *aux, SLboolean *enable) {
    (void)aux;
    return es_is_enabled(self, enable);
}
static SLresult es_SetDirectLevel(SLEffectSendItf self, SLmillibel level) { return es_set_level(self, level, 1); }
static SLresult es_GetDirectLevel(SLEffectSendItf self, SLmillibel *level) { return es_get_level(self, level, 1); }
static SLresult es_SetSendLevel(SLEffectSendItf self, const void *aux, SLmillibel level) {
    (void)aux;
    return es_set_level(self, level, 0);
}
static SLresult es_GetSendLevel(SLEffectSendItf self, const void *aux, SLmillibel *level) {
    (void)aux;
    return es_get_level(self, level, 0);
}

static const struct SLEffectSendItf_ effectsend_vtable = {
    .EnableEffectSend = es_EnableEffectSend,
    .IsEnabled = es_IsEnabled,
    .SetDirectLevel = es_SetDirectLevel,
    .GetDirectLevel = es_GetDirectLevel,
    .SetSendLevel = es_SetSendLevel,
    .GetSendLevel = es_GetSendLevel,
};

static SLresult aes_EnableEffect(SLAndroidEffectSendItf self, SLInterfaceID effect, SLboolean enable,
                                 SLmillibel level) {
    (void)effect;
    return es_enable(self, enable, level);
}
static SLresult aes_IsEnabled(SLAndroidEffectSendItf self, SLInterfaceID effect, SLboolean *enable) {
    (void)effect;
    return es_is_enabled(self, enable);
}
static SLresult aes_SetDirectLevel(SLAndroidEffectSendItf self, SLmillibel level) { return es_set_level(self, level, 1); }
static SLresult aes_GetDirectLevel(SLAndroidEffectSendItf self, SLmillibel *level) { return es_get_level(self, level, 1); }
static SLresult aes_SetSendLevel(SLAndroidEffectSendItf self, SLInterfaceID effect, SLmillibel level) {
    (void)effect;
    return es_set_level(self, level, 0);
}
static SLresult aes_GetSendLevel(SLAndroidEffectSendItf self, SLInterfaceID effect, SLmillibel *level) {
    (void)effect;
    return es_get_level(self, level, 0);
}

static const struct SLAndroidEffectSendItf_ androideffectsend_vtable = {
    .EnableEffectSend = aes_EnableEffect,
    .IsEnabled = aes_IsEnabled,
    .SetDirectLevel = aes_SetDirectLevel,
    .GetDirectLevel = aes_GetDirectLevel,
    .SetSendLevel = aes_SetSendLevel,
    .GetSendLevel = aes_GetSendLevel,
};

/* ---- SLAndroidConfigurationItf ------------------------------------------------------- */
/* Stream type, performance mode and the like are accepted and ignored. */

static SLresult cfg_SetConfiguration(SLAndroidConfigurationItf self, const SLchar *key, const void *value,
                                     SLuint32 size) {
    (void)self;
    if (!key || !value || size == 0) return SL_RESULT_PARAMETER_INVALID;
    return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(SLAndroidConfigurationItf self, const SLchar *key, SLuint32 *size,
                                     void *value) {
    (void)self, (void)value;
    if (!key || !size) return SL_RESULT_PARAMETER_INVALID;
    return SL_RESULT_PARAMETER_INVALID;
}

static const struct SLAndroidConfigurationItf_ config_vtable = {
    .SetConfiguration = cfg_SetConfiguration,
    .GetConfiguration = cfg_GetConfiguration,
};

/* ---- SLOutputMixItf ------------------------------------------------------------------ */

static SLresult mix_GetDestinationOutputDeviceIDs(SLOutputMixItf self, SLint32 *n, SLuint32 *ids) {
    (void)self;
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    if (ids && *n >= 1) ids[0] = SL_DEFAULTDEVICEID_AUDIOOUTPUT;
    *n = 1;
    return SL_RESULT_SUCCESS;
}
static SLresult mix_RegisterDeviceChangeCallback(SLOutputMixItf self, slMixDeviceChangeCallback cb, void *ctx) {
    (void)self, (void)cb, (void)ctx;
    return SL_RESULT_SUCCESS;
}
static SLresult mix_ReRoute(SLOutputMixItf self, SLint32 n, SLuint32 *ids) {
    (void)self, (void)n, (void)ids;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLOutputMixItf_ outputmix_vtable = {
    .GetDestinationOutputDeviceIDs = mix_GetDestinationOutputDeviceIDs,
    .RegisterDeviceChangeCallback = mix_RegisterDeviceChangeCallback,
    .ReRoute = mix_ReRoute,
};

/* ---- interface lookup ---------------------------------------------------------------- */

static zb_itf *find_interface(zb_obj *o, SLInterfaceID iid) {
    if (iid_is(iid, SL_IID_OBJECT)) return &o->object;
    switch (o->kind) {
    case KIND_ENGINE:
        if (iid_is(iid, SL_IID_ENGINE)) return &o->engine;
        return NULL;
    case KIND_OUTPUTMIX:
        if (iid_is(iid, SL_IID_OUTPUTMIX)) return &o->outputmix;
        return NULL;
    case KIND_PLAYER:
        if (iid_is(iid, SL_IID_PLAY)) return &o->play;
        if (iid_is(iid, SL_IID_VOLUME)) return &o->volume;
        if (iid_is(iid, SL_IID_MUTESOLO)) return o->channels > 0 ? &o->mutesolo : NULL;
        if (iid_is(iid, SL_IID_EFFECTSEND)) return &o->effectsend;
        if (iid_is(iid, SL_IID_ANDROIDEFFECTSEND)) return &o->androideffectsend;
        if (iid_is(iid, SL_IID_ANDROIDCONFIGURATION)) return &o->config;
        if (iid_is(iid, SL_IID_PLAYBACKRATE)) return &o->rate;
        if (o->source == SOURCE_BUFFERQUEUE) {
            if (iid_is(iid, SL_IID_BUFFERQUEUE)) return &o->bufferqueue;
            if (iid_is(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) return &o->simplebufferqueue;
        } else {
            if (iid_is(iid, SL_IID_SEEK)) return &o->seek;
            if (iid_is(iid, SL_IID_PREFETCHSTATUS)) return &o->prefetch;
        }
        return NULL;
    }
    return NULL;
}

static SLresult object_get_interface_locked(zb_obj *o, SLInterfaceID iid, void *out) {
    zb_itf *itf = find_interface(o, iid);
    if (!itf) return SL_RESULT_FEATURE_UNSUPPORTED;
    *(void **)out = itf;
    return SL_RESULT_SUCCESS;
}

/* Every required interface must exist; optional ones that do not are simply absent. */
static SLresult check_interfaces(zb_obj *o, SLuint32 n, const SLInterfaceID *ids, const SLboolean *required) {
    if (n > 0 && !ids) return SL_RESULT_PARAMETER_INVALID;
    for (SLuint32 i = 0; i < n; i++) {
        if (!ids[i]) return SL_RESULT_PARAMETER_INVALID;
        if (!find_interface(o, ids[i]) && required && required[i]) return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    return SL_RESULT_SUCCESS;
}

static zb_obj *new_object(int kind) {
    zb_obj *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->object.vtable = &object_vtable;
    o->object.owner = o;
    return o;
}

static void publish_object(zb_obj *o, SLObjectItf *out) {
    pthread_mutex_lock(&g_lock);
    o->next = g_objects;
    g_objects = o;
    pthread_mutex_unlock(&g_lock);
    *out = (SLObjectItf)&o->object;
}

#define INIT_ITF(o, field, table) ((o)->field.vtable = &(table), (o)->field.owner = (o))

/* ---- SLEngineItf --------------------------------------------------------------------- */

static int g_warned_led, g_warned_vibra, g_warned_recorder, g_warned_midi, g_warned_3d,
    g_warned_metadata, g_warned_extension, g_warned_decoder, g_warned_source;

static SLresult eng_CreateLEDDevice(SLEngineItf self, SLObjectItf *dev, SLuint32 id, SLuint32 n,
                                    const SLInterfaceID *ids, const SLboolean *req) {
    (void)self, (void)dev, (void)id, (void)n, (void)ids, (void)req;
    log_once(&g_warned_led, "LED device");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateVibraDevice(SLEngineItf self, SLObjectItf *dev, SLuint32 id, SLuint32 n,
                                      const SLInterfaceID *ids, const SLboolean *req) {
    (void)self, (void)dev, (void)id, (void)n, (void)ids, (void)req;
    log_once(&g_warned_vibra, "vibra device");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static int is_buffer_queue_locator(const void *locator) {
    SLuint32 type = *(const SLuint32 *)locator;
    return type == SL_DATALOCATOR_BUFFERQUEUE || type == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
}

static SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *out, SLDataSource *src, SLDataSink *sink,
                                      SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    (void)self;
    if (!out || !src || !src->pLocator || !sink || !sink->pLocator) return SL_RESULT_PARAMETER_INVALID;
    if (is_buffer_queue_locator(sink->pLocator)) {
        log_once(&g_warned_decoder, "decode to buffer queue");
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    if (*(const SLuint32 *)sink->pLocator != SL_DATALOCATOR_OUTPUTMIX) return SL_RESULT_PARAMETER_INVALID;

    zb_obj *p = new_object(KIND_PLAYER);
    if (!p) return SL_RESULT_MEMORY_FAILURE;
    SLuint32 locator = *(const SLuint32 *)src->pLocator;
    if (is_buffer_queue_locator(src->pLocator)) {
        const SLDataLocator_BufferQueue *bq = src->pLocator;
        const SLDataFormat_PCM *pcm = src->pFormat;
        if (!pcm || (pcm->formatType != SL_DATAFORMAT_PCM && pcm->formatType != SL_ANDROID_DATAFORMAT_PCM_EX) ||
            pcm->numChannels == 0 || pcm->numChannels > 8 || pcm->samplesPerSec == 0 || bq->numBuffers == 0) {
            free(p);
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        SLuint32 container = pcm->containerSize ? pcm->containerSize : pcm->bitsPerSample;
        uint64_t bytes = (uint64_t)pcm->samplesPerSec / 1000 * pcm->numChannels * ((container + 7) / 8);
        p->source = SOURCE_BUFFERQUEUE;
        p->capacity = bq->numBuffers;
        p->bytes_per_second = bytes > 0 && bytes < UINT32_MAX ? (uint32_t)bytes : 0;
        p->channels = pcm->numChannels;
        p->sample_rate_hz = pcm->samplesPerSec / 1000;
        /* What the host can play: 8-bit unsigned and 16-bit signed integers, 32-bit floats. */
        /* SLAndroidDataFormat_PCM_EX is SLDataFormat_PCM plus a trailing representation word. */
        const int is_float = pcm->formatType == SL_ANDROID_DATAFORMAT_PCM_EX &&
                             ((const SLuint32 *)pcm)[7] == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
        if (pcm->endianness != SL_BYTEORDER_BIGENDIAN) {
            if (pcm->bitsPerSample == 8 && container == 8 && !is_float) p->encoding = ZB_AUDIO_ENCODING_U8;
            if (pcm->bitsPerSample == 16 && container == 16 && !is_float) p->encoding = ZB_AUDIO_ENCODING_S16;
            if (pcm->bitsPerSample == 32 && container == 32 && is_float) p->encoding = ZB_AUDIO_ENCODING_FLOAT;
        }
        p->sizes = calloc(p->capacity, sizeof(*p->sizes));
        p->buffers = calloc(p->capacity, sizeof(*p->buffers));
        if (!p->sizes || !p->buffers || p->bytes_per_second == 0) {
            SLresult r = p->sizes && p->buffers ? SL_RESULT_CONTENT_UNSUPPORTED : SL_RESULT_MEMORY_FAILURE;
            free(p->sizes);
            free(p->buffers);
            free(p);
            return r;
        }
    } else if (locator == SL_DATALOCATOR_URI || locator == SL_DATALOCATOR_ANDROIDFD ||
               locator == SL_DATALOCATOR_ADDRESS) {
        p->source = SOURCE_MEDIA;
        /* Remembered for Realize, where the host player is created: the locator itself is the
         * caller's and may be gone by then. An address source stays silent. */
        if (locator == SL_DATALOCATOR_ANDROIDFD) {
            const SLDataLocator_AndroidFD *fd = src->pLocator;
            const uint64_t offset = (uint64_t)fd->offset;
            const uint64_t length = (uint64_t)fd->length;
            p->media_source.kind = ZB_AUDIO_MEDIA_FD;
            p->media_source.fd = fd->fd;
            p->media_source.offset_lo = (uint32_t)offset;
            p->media_source.offset_hi = (uint32_t)(offset >> 32);
            p->media_source.length_lo = (uint32_t)length;
            p->media_source.length_hi = (uint32_t)(length >> 32);
        } else if (locator == SL_DATALOCATOR_URI) {
            const SLDataLocator_URI *uri = src->pLocator;
            if (uri->URI) {
                p->uri = strdup((const char *)uri->URI);
                if (!p->uri) {
                    free(p);
                    return SL_RESULT_MEMORY_FAILURE;
                }
                p->media_source.kind = ZB_AUDIO_MEDIA_URI;
            }
        }
    } else {
        log_once(&g_warned_source, "audio player data source");
        free(p);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }

    INIT_ITF(p, play, play_vtable);
    INIT_ITF(p, bufferqueue, bufferqueue_vtable);
    INIT_ITF(p, simplebufferqueue, simplebufferqueue_vtable);
    INIT_ITF(p, volume, volume_vtable);
    INIT_ITF(p, seek, seek_vtable);
    INIT_ITF(p, prefetch, prefetch_vtable);
    INIT_ITF(p, mutesolo, mutesolo_vtable);
    INIT_ITF(p, rate, rate_vtable);
    INIT_ITF(p, effectsend, effectsend_vtable);
    INIT_ITF(p, androideffectsend, androideffectsend_vtable);
    INIT_ITF(p, config, config_vtable);
    pthread_cond_init(&p->out_wake, NULL);
    p->play_state = SL_PLAYSTATE_STOPPED;
    p->update_period = 1000;
    p->next_newpos = 1000;
    p->playback_rate = 1000;
    p->loop_end = SL_TIME_UNKNOWN;

    SLresult r = check_interfaces(p, n, ids, req);
    if (r != SL_RESULT_SUCCESS) {
        free_object(p);
        return r;
    }
    publish_object(p, out);
    return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateAudioRecorder(SLEngineItf self, SLObjectItf *rec, SLDataSource *src, SLDataSink *sink,
                                        SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    (void)self, (void)rec, (void)src, (void)sink, (void)n, (void)ids, (void)req;
    log_once(&g_warned_recorder, "audio recorder");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateMidiPlayer(SLEngineItf self, SLObjectItf *player, SLDataSource *midi, SLDataSource *bank,
                                     SLDataSink *out, SLDataSink *vibra, SLDataSink *led, SLuint32 n,
                                     const SLInterfaceID *ids, const SLboolean *req) {
    (void)self, (void)player, (void)midi, (void)bank, (void)out, (void)vibra, (void)led, (void)n, (void)ids,
        (void)req;
    log_once(&g_warned_midi, "MIDI player");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateListener(SLEngineItf self, SLObjectItf *listener, SLuint32 n, const SLInterfaceID *ids,
                                   const SLboolean *req) {
    (void)self, (void)listener, (void)n, (void)ids, (void)req;
    log_once(&g_warned_3d, "3D listener/group");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_Create3DGroup(SLEngineItf self, SLObjectItf *group, SLuint32 n, const SLInterfaceID *ids,
                                  const SLboolean *req) {
    (void)self, (void)group, (void)n, (void)ids, (void)req;
    log_once(&g_warned_3d, "3D listener/group");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateOutputMix(SLEngineItf self, SLObjectItf *out, SLuint32 n, const SLInterfaceID *ids,
                                    const SLboolean *req) {
    (void)self;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    zb_obj *m = new_object(KIND_OUTPUTMIX);
    if (!m) return SL_RESULT_MEMORY_FAILURE;
    INIT_ITF(m, outputmix, outputmix_vtable);
    SLresult r = check_interfaces(m, n, ids, req);
    if (r != SL_RESULT_SUCCESS) {
        free(m);
        return r;
    }
    publish_object(m, out);
    return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateMetadataExtractor(SLEngineItf self, SLObjectItf *out, SLDataSource *src, SLuint32 n,
                                            const SLInterfaceID *ids, const SLboolean *req) {
    (void)self, (void)out, (void)src, (void)n, (void)ids, (void)req;
    log_once(&g_warned_metadata, "metadata extractor");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateExtensionObject(SLEngineItf self, SLObjectItf *out, void *params, SLuint32 id,
                                          SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    (void)self, (void)out, (void)params, (void)id, (void)n, (void)ids, (void)req;
    log_once(&g_warned_extension, "extension object");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

/* Interfaces reported by Query*SupportedInterfaces, per object type. */
static const SLInterfaceID *supported_list(SLuint32 object_id, SLuint32 *count) {
    static SLInterfaceID engine[2], mix[2], player[12];
    static int ready;
    if (!ready) {
        SLInterfaceID e[] = {SL_IID_OBJECT, SL_IID_ENGINE};
        SLInterfaceID m[] = {SL_IID_OBJECT, SL_IID_OUTPUTMIX};
        SLInterfaceID p[] = {SL_IID_OBJECT, SL_IID_PLAY, SL_IID_BUFFERQUEUE, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                             SL_IID_VOLUME, SL_IID_SEEK, SL_IID_PREFETCHSTATUS, SL_IID_MUTESOLO,
                             SL_IID_PLAYBACKRATE, SL_IID_EFFECTSEND, SL_IID_ANDROIDEFFECTSEND,
                             SL_IID_ANDROIDCONFIGURATION};
        memcpy(engine, e, sizeof(e));
        memcpy(mix, m, sizeof(m));
        memcpy(player, p, sizeof(p));
        ready = 1;
    }
    switch (object_id) {
    case SL_OBJECTID_ENGINE: *count = 2; return engine;
    case SL_OBJECTID_OUTPUTMIX: *count = 2; return mix;
    case SL_OBJECTID_AUDIOPLAYER: *count = 12; return player;
    }
    *count = 0;
    return NULL;
}

static SLresult eng_QueryNumSupportedInterfaces(SLEngineItf self, SLuint32 object_id, SLuint32 *n) {
    (void)self;
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    const SLInterfaceID *list = supported_list(object_id, n);
    pthread_mutex_unlock(&g_lock);
    return list ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_QuerySupportedInterfaces(SLEngineItf self, SLuint32 object_id, SLuint32 index,
                                             SLInterfaceID *iid) {
    (void)self;
    if (!iid) return SL_RESULT_PARAMETER_INVALID;
    SLuint32 n;
    pthread_mutex_lock(&g_lock);
    const SLInterfaceID *list = supported_list(object_id, &n);
    pthread_mutex_unlock(&g_lock);
    if (!list) return SL_RESULT_FEATURE_UNSUPPORTED;
    if (index >= n) return SL_RESULT_PARAMETER_INVALID;
    *iid = list[index];
    return SL_RESULT_SUCCESS;
}

static SLresult eng_QueryNumSupportedExtensions(SLEngineItf self, SLuint32 *n) {
    (void)self;
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    *n = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_QuerySupportedExtension(SLEngineItf self, SLuint32 index, SLchar *name, SLint16 *length) {
    (void)self, (void)index, (void)name, (void)length;
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult eng_IsExtensionSupported(SLEngineItf self, const SLchar *name, SLboolean *supported) {
    (void)self;
    if (!name || !supported) return SL_RESULT_PARAMETER_INVALID;
    *supported = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vtable = {
    .CreateLEDDevice = eng_CreateLEDDevice,
    .CreateVibraDevice = eng_CreateVibraDevice,
    .CreateAudioPlayer = eng_CreateAudioPlayer,
    .CreateAudioRecorder = eng_CreateAudioRecorder,
    .CreateMidiPlayer = eng_CreateMidiPlayer,
    .CreateListener = eng_CreateListener,
    .Create3DGroup = eng_Create3DGroup,
    .CreateOutputMix = eng_CreateOutputMix,
    .CreateMetadataExtractor = eng_CreateMetadataExtractor,
    .CreateExtensionObject = eng_CreateExtensionObject,
    .QueryNumSupportedInterfaces = eng_QueryNumSupportedInterfaces,
    .QuerySupportedInterfaces = eng_QuerySupportedInterfaces,
    .QueryNumSupportedExtensions = eng_QueryNumSupportedExtensions,
    .QuerySupportedExtension = eng_QuerySupportedExtension,
    .IsExtensionSupported = eng_IsExtensionSupported,
};

/* ---- entry points -------------------------------------------------------------------- */

ZB_EXPORT SLresult slCreateEngine(SLObjectItf *out, SLuint32 num_options, const SLEngineOption *options,
                                  SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    if (!out || (num_options > 0 && !options)) return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    zb_obj *e = new_object(KIND_ENGINE);
    if (!e) return SL_RESULT_MEMORY_FAILURE;
    INIT_ITF(e, engine, engine_vtable);
    SLresult r = check_interfaces(e, n, ids, req);
    if (r != SL_RESULT_SUCCESS) {
        free(e);
        return r;
    }
    publish_object(e, out);
    return SL_RESULT_SUCCESS;
}

ZB_EXPORT SLresult slQueryNumSupportedEngineInterfaces(SLuint32 *n) {
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    supported_list(SL_OBJECTID_ENGINE, n);
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

ZB_EXPORT SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID *iid) {
    if (!iid) return SL_RESULT_PARAMETER_INVALID;
    SLuint32 n;
    pthread_mutex_lock(&g_lock);
    const SLInterfaceID *list = supported_list(SL_OBJECTID_ENGINE, &n);
    pthread_mutex_unlock(&g_lock);
    if (index >= n) return SL_RESULT_PARAMETER_INVALID;
    *iid = list[index];
    return SL_RESULT_SUCCESS;
}
