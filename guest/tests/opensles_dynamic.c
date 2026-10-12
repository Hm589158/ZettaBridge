/* Drives the guest libOpenSLES.so (guest/opensles/zbopensles.c) the way games do: engine,
 * output mix, a PCM buffer queue player paced by its callback, a URI player waiting for
 * prefetch, and the refusals. Run with LD_LIBRARY_PATH pointing at build/guest/lib. */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int bq_calls;
static int realize_calls;
static int prefetch_calls;
static SLuint32 prefetch_mask;
static short pcm[800]; /* 1600 bytes: 100 ms of 8 kHz mono 16-bit */

static void bq_callback(SLAndroidSimpleBufferQueueItf bq, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&lock);
    bq_calls++;
    pthread_mutex_unlock(&lock);
    (*bq)->Enqueue(bq, pcm, sizeof(pcm));
}

static void object_callback(SLObjectItf obj, const void *ctx, SLuint32 event, SLresult result, SLuint32 param,
                            void *itf) {
    (void)obj, (void)ctx, (void)param, (void)itf;
    if (event == SL_OBJECT_EVENT_ASYNC_TERMINATION && result == SL_RESULT_SUCCESS) {
        pthread_mutex_lock(&lock);
        realize_calls++;
        pthread_mutex_unlock(&lock);
    }
}

static void prefetch_callback(SLPrefetchStatusItf itf, void *ctx, SLuint32 event) {
    (void)itf, (void)ctx;
    pthread_mutex_lock(&lock);
    prefetch_calls++;
    prefetch_mask = event;
    pthread_mutex_unlock(&lock);
}

static int locked_read(int *value) {
    pthread_mutex_lock(&lock);
    int v = *value;
    pthread_mutex_unlock(&lock);
    return v;
}

int main(void) {
    SLObjectItf engine_obj = NULL;
    SLEngineItf engine;
    check(slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL) == SL_RESULT_SUCCESS, "create engine");
    check((*engine_obj)->Realize(engine_obj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "realize engine");
    check((*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS, "engine itf");
    SLuint32 n = 0;
    check(slQueryNumSupportedEngineInterfaces(&n) == SL_RESULT_SUCCESS && n > 0, "query engine interfaces");

    /* Output mix: reverb is optional in the NDK sample and must be skippable. */
    SLObjectItf mix_obj = NULL;
    const SLInterfaceID reverb[] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean optional[] = {SL_BOOLEAN_FALSE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    check((*engine)->CreateOutputMix(engine, &mix_obj, 1, reverb, required) == SL_RESULT_FEATURE_UNSUPPORTED,
          "required reverb refused");
    check((*engine)->CreateOutputMix(engine, &mix_obj, 1, reverb, optional) == SL_RESULT_SUCCESS,
          "optional reverb skipped");
    (*mix_obj)->RegisterCallback(mix_obj, object_callback, NULL);
    check((*mix_obj)->Realize(mix_obj, SL_BOOLEAN_TRUE) == SL_RESULT_SUCCESS, "async realize");
    void *unused;
    check((*mix_obj)->GetInterface(mix_obj, SL_IID_ENVIRONMENTALREVERB, &unused) == SL_RESULT_FEATURE_UNSUPPORTED,
          "no reverb interface");

    /* Buffer queue player. */
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_8, SL_PCMSAMPLEFORMAT_FIXED_16,
                               SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc_bq, &format};
    SLDataLocator_OutputMix loc_mix = {SL_DATALOCATOR_OUTPUTMIX, mix_obj};
    SLDataSink sink = {&loc_mix, NULL};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME, SL_IID_EFFECTSEND};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    SLObjectItf player_obj = NULL;
    check((*engine)->CreateAudioPlayer(engine, &player_obj, &src, &sink, 3, ids, req) == SL_RESULT_SUCCESS,
          "create bq player");
    check((*player_obj)->Realize(player_obj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "realize bq player");
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf bq;
    SLVolumeItf volume;
    check((*player_obj)->GetInterface(player_obj, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS, "play itf");
    check((*player_obj)->GetInterface(player_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bq) == SL_RESULT_SUCCESS,
          "bq itf");
    check((*player_obj)->GetInterface(player_obj, SL_IID_VOLUME, &volume) == SL_RESULT_SUCCESS, "volume itf");
    check((*volume)->SetVolumeLevel(volume, -300) == SL_RESULT_SUCCESS, "set volume");
    check((*bq)->RegisterCallback(bq, bq_callback, NULL) == SL_RESULT_SUCCESS, "register bq callback");
    check((*bq)->Enqueue(bq, pcm, sizeof(pcm)) == SL_RESULT_SUCCESS, "enqueue 1");
    check((*bq)->Enqueue(bq, pcm, sizeof(pcm)) == SL_RESULT_SUCCESS, "enqueue 2");
    check((*bq)->Enqueue(bq, pcm, sizeof(pcm)) == SL_RESULT_BUFFER_INSUFFICIENT, "queue full");
    sleep_ms(250);
    check(locked_read(&bq_calls) == 0, "no callbacks while stopped");
    check((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS, "play");
    sleep_ms(1000);
    check((*play)->SetPlayState(play, SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS, "pause");
    int calls = locked_read(&bq_calls);
    /* 100 ms buffers over about 1 s; translated code may lag, so the window is wide. */
    check(calls >= 5 && calls <= 12, "callbacks paced in real time");
    sleep_ms(300);
    check(locked_read(&bq_calls) == calls, "no callbacks while paused");
    SLAndroidSimpleBufferQueueState state;
    check((*bq)->GetState(bq, &state) == SL_RESULT_SUCCESS && state.count == 2 && state.index == (SLuint32)calls,
          "bq state");
    SLmillisecond pos = 0;
    check((*play)->GetPosition(play, &pos) == SL_RESULT_SUCCESS && pos == (SLmillisecond)calls * 100,
          "bq position");
    (*player_obj)->Destroy(player_obj);
    printf("bq player destroyed\n");

    /* URI player: games wait for SUFFICIENTDATA after PAUSED before reading the duration. */
    SLDataLocator_URI loc_uri = {SL_DATALOCATOR_URI, (SLchar *)"file:///sdcard/music.ogg"};
    SLDataFormat_MIME mime = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED};
    SLDataSource uri_src = {&loc_uri, &mime};
    const SLInterfaceID uri_ids[] = {SL_IID_SEEK, SL_IID_PREFETCHSTATUS};
    const SLboolean uri_req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    SLObjectItf uri_obj = NULL;
    check((*engine)->CreateAudioPlayer(engine, &uri_obj, &uri_src, &sink, 2, uri_ids, uri_req) ==
              SL_RESULT_SUCCESS,
          "create uri player");
    (*uri_obj)->Realize(uri_obj, SL_BOOLEAN_FALSE);
    SLPlayItf uri_play;
    SLPrefetchStatusItf prefetch;
    SLSeekItf seek;
    (*uri_obj)->GetInterface(uri_obj, SL_IID_PLAY, &uri_play);
    check((*uri_obj)->GetInterface(uri_obj, SL_IID_PREFETCHSTATUS, &prefetch) == SL_RESULT_SUCCESS, "prefetch itf");
    check((*uri_obj)->GetInterface(uri_obj, SL_IID_SEEK, &seek) == SL_RESULT_SUCCESS, "seek itf");
    check((*seek)->SetLoop(seek, SL_BOOLEAN_TRUE, 0, SL_TIME_UNKNOWN) == SL_RESULT_SUCCESS, "loop");
    (*prefetch)->RegisterCallback(prefetch, prefetch_callback, NULL);
    (*prefetch)->SetCallbackEventsMask(prefetch, SL_PREFETCHEVENT_STATUSCHANGE);
    (*uri_play)->SetPlayState(uri_play, SL_PLAYSTATE_PAUSED);
    for (int i = 0; i < 100 && locked_read(&prefetch_calls) == 0; i++) sleep_ms(10);
    check(locked_read(&prefetch_calls) == 1 && prefetch_mask == SL_PREFETCHEVENT_STATUSCHANGE, "prefetch event");
    SLuint32 status = 0;
    (*prefetch)->GetPrefetchStatus(prefetch, &status);
    check(status == SL_PREFETCHSTATUS_SUFFICIENTDATA, "sufficient data");
    SLmillisecond duration = 0;
    (*uri_play)->GetDuration(uri_play, &duration);
    check(duration == SL_TIME_UNKNOWN, "duration unknown");
    (*uri_play)->SetPlayState(uri_play, SL_PLAYSTATE_PLAYING);
    sleep_ms(200);
    (*uri_play)->GetPosition(uri_play, &pos);
    check(pos >= 100, "uri position advances");
    (*uri_obj)->Destroy(uri_obj);

    /* Decoding to a buffer queue would never deliver PCM, so it is refused. */
    SLDataSink decode_sink = {&loc_bq, &format};
    SLObjectItf decoder = NULL;
    check((*engine)->CreateAudioPlayer(engine, &decoder, &uri_src, &decode_sink, 0, NULL, NULL) ==
              SL_RESULT_CONTENT_UNSUPPORTED,
          "decoder refused");
    check((*engine)->CreateAudioRecorder(engine, &decoder, &src, &sink, 0, NULL, NULL) ==
              SL_RESULT_FEATURE_UNSUPPORTED,
          "recorder refused");

    check(locked_read(&realize_calls) == 1, "async realize callback");
    (*mix_obj)->Destroy(mix_obj);
    (*engine_obj)->Destroy(engine_obj);
    printf("done\n");
    return failures;
}
