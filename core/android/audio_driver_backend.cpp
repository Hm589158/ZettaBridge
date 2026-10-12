#include "audio_driver_backend.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "zb/audio_protocol.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// How long one AAudio write may wait before the writer checks whether it was cancelled.
constexpr std::int64_t kWriteSliceNanos = 20'000'000;
// Frames converted per write slice when the samples need conversion or gain.
constexpr std::size_t kChunkFrames = 1024;

// The report is the only record that survives a device run (OxygenOS drops our logcat).
void note(const std::string& key, const std::string& value, bool overwrite = true) {
    runtime_report().note_jni_detail("audio-" + key, value, overwrite);
}

std::uint32_t input_sample_bytes(AudioBackend::Encoding encoding) {
    switch (encoding) {
    case AudioBackend::Encoding::kU8: return 1;
    case AudioBackend::Encoding::kS16: return 2;
    case AudioBackend::Encoding::kFloat: return 4;
    }
    return 2;
}

void sleep_nanos(std::int64_t nanos) {
    if (nanos <= 0) return;
    timespec ts{static_cast<time_t>(nanos / 1'000'000'000), static_cast<long>(nanos % 1'000'000'000)};
    nanosleep(&ts, nullptr);
}

}  // namespace

struct AudioDriverBackend::Pcm {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    Encoding encoding = Encoding::kS16;
    std::atomic<float> gain{1.0f};
    // Bumped by pause, flush and close: a write in progress returns early when it changes.
    std::atomic<std::uint32_t> generation{0};
    std::mutex lifecycle;  // serializes the AAudio state calls and a reopen
    AAudioStream* stream = nullptr;
    bool started = false;
    std::vector<std::uint8_t> scratch;

    ~Pcm() {
        if (stream != nullptr) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
        }
    }

    // Caller holds lifecycle.
    bool open() {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<std::int32_t>(sample_rate));
        AAudioStreamBuilder_setChannelCount(builder, static_cast<std::int32_t>(channels));
        // 8-bit PCM has no AAudio format: it is widened to 16 bits on the way.
        AAudioStreamBuilder_setFormat(builder, encoding == Encoding::kFloat ? AAUDIO_FORMAT_PCM_FLOAT
                                                                            : AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SONIFICATION);
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK) {
            note("pcm-open-error", std::string(AAudio_convertResultToText(result)) + " rate=" +
                                       std::to_string(sample_rate) + " channels=" + std::to_string(channels));
            stream = nullptr;
            return false;
        }
        return true;
    }

    // Caller holds lifecycle. A disconnected stream (headphones, Bluetooth) cannot recover: AAudio
    // wants a new one with the same parameters.
    bool reopen() {
        if (stream != nullptr) {
            AAudioStream_close(stream);
            stream = nullptr;
        }
        if (!open()) return false;
        if (started) AAudioStream_requestStart(stream);
        return true;
    }
};

struct AudioDriverBackend::Media {
    SLObjectItf player = nullptr;
    SLPlayItf play = nullptr;
    SLSeekItf seek = nullptr;
    SLVolumeItf volume = nullptr;
    SLPrefetchStatusItf prefetch = nullptr;
    std::atomic<std::uint32_t> status{0};

    ~Media() {
        // Destroy waits for callbacks in flight, which only touch `status`.
        if (player != nullptr) (*player)->Destroy(player);
    }
};

namespace {

void SLAPIENTRY media_play_event(SLPlayItf, void* context, SLuint32 event) {
    auto* media = static_cast<AudioDriverBackend::Media*>(context);
    if (event & SL_PLAYEVENT_HEADATEND) media->status.fetch_or(ZB_AUDIO_STATUS_ENDED);
}

void SLAPIENTRY media_prefetch_event(SLPrefetchStatusItf caller, void* context, SLuint32) {
    auto* media = static_cast<AudioDriverBackend::Media*>(context);
    SLuint32 status = 0;
    SLpermille level = 0;
    (*caller)->GetPrefetchStatus(caller, &status);
    (*caller)->GetFillLevel(caller, &level);
    // Android reports content it cannot open or decode as an underflow with nothing buffered.
    if (status == SL_PREFETCHSTATUS_UNDERFLOW && level == 0) {
        media->status.fetch_or(ZB_AUDIO_STATUS_ERROR);
    } else if (status == SL_PREFETCHSTATUS_SUFFICIENTDATA) {
        media->status.fetch_or(ZB_AUDIO_STATUS_PREFETCHED);
    }
}

SLmillibel millibel(float gain) {
    if (!(gain > 0.0f)) return SL_MILLIBEL_MIN;
    const float level = 2000.0f * std::log10(std::min(gain, 1.0f));
    return static_cast<SLmillibel>(std::max(level, static_cast<float>(SL_MILLIBEL_MIN)));
}

}  // namespace

AudioDriverBackend::~AudioDriverBackend() {
    // Process-lifetime in practice (the guest runtime never shuts down); release in order anyway.
    medias_.clear();
    pcms_.clear();
    if (output_mix_ != nullptr) {
        auto mix = static_cast<SLObjectItf>(output_mix_);
        (*mix)->Destroy(mix);
    }
    if (engine_object_ != nullptr) {
        auto object = static_cast<SLObjectItf>(engine_object_);
        (*object)->Destroy(object);
    }
}

std::shared_ptr<AudioDriverBackend::Pcm> AudioDriverBackend::pcm(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pcms_.find(id);
    return found == pcms_.end() ? nullptr : found->second;
}

std::shared_ptr<AudioDriverBackend::Media> AudioDriverBackend::media(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = medias_.find(id);
    return found == medias_.end() ? nullptr : found->second;
}

std::uint64_t AudioDriverBackend::pcm_open(std::uint32_t sample_rate, std::uint32_t channels, Encoding encoding) {
    auto stream = std::make_shared<Pcm>();
    stream->sample_rate = sample_rate;
    stream->channels = channels;
    stream->encoding = encoding;
    {
        std::lock_guard<std::mutex> lock(stream->lifecycle);
        if (!stream->open()) return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t id = next_id_++;
    pcms_[id] = std::move(stream);
    note("pcm-streams", std::to_string(pcms_.size()) + " open, last " + std::to_string(sample_rate) + " Hz x" +
                            std::to_string(channels));
    return id;
}

std::uint32_t AudioDriverBackend::pcm_write(std::uint64_t id, const void* data, std::uint32_t bytes) {
    std::shared_ptr<Pcm> stream = pcm(id);
    if (stream == nullptr) return 0;
    const std::uint32_t sample_bytes = input_sample_bytes(stream->encoding);
    const std::uint32_t frame_bytes = sample_bytes * stream->channels;
    const std::uint64_t frames = bytes / frame_bytes;
    const std::uint32_t generation = stream->generation.load();
    const auto* input = static_cast<const std::uint8_t*>(data);
    std::uint64_t done = 0;
    while (done < frames) {
        if (stream->generation.load() != generation) break;
        const std::uint64_t count = std::min<std::uint64_t>(frames - done, kChunkFrames);
        const std::uint8_t* chunk = input + done * frame_bytes;
        const float gain = stream->gain.load();
        const void* samples = chunk;
        // 8-bit samples are widened, and a gain below one is applied here: AAudio has no stream
        // volume of its own.
        if (stream->encoding == Encoding::kU8 || gain < 1.0f) {
            const std::size_t values = static_cast<std::size_t>(count) * stream->channels;
            stream->scratch.resize(values * (stream->encoding == Encoding::kFloat ? 4 : 2));
            if (stream->encoding == Encoding::kFloat) {
                auto* out = reinterpret_cast<float*>(stream->scratch.data());
                std::memcpy(out, chunk, values * 4);
                for (std::size_t i = 0; i < values; ++i) out[i] *= gain;
            } else {
                auto* out = reinterpret_cast<std::int16_t*>(stream->scratch.data());
                for (std::size_t i = 0; i < values; ++i) {
                    float sample;
                    if (stream->encoding == Encoding::kU8) {
                        sample = static_cast<float>((static_cast<int>(chunk[i]) - 128) * 256);
                    } else {
                        std::int16_t value;
                        std::memcpy(&value, chunk + i * 2, 2);
                        sample = static_cast<float>(value);
                    }
                    out[i] = static_cast<std::int16_t>(std::clamp(sample * gain, -32768.0f, 32767.0f));
                }
            }
            samples = stream->scratch.data();
        }
        aaudio_result_t written = stream->stream == nullptr
                                      ? AAUDIO_ERROR_DISCONNECTED
                                      : AAudioStream_write(stream->stream, samples, static_cast<std::int32_t>(count),
                                                           kWriteSliceNanos);
        if (written == AAUDIO_ERROR_DISCONNECTED) {
            std::lock_guard<std::mutex> lock(stream->lifecycle);
            note("pcm-reconnects", "stream disconnected, reopened", true);
            if (stream->reopen()) continue;
            written = AAUDIO_ERROR_INVALID_STATE;
        }
        if (written < 0) {
            // A stream that keeps failing still has to pace the guest, or its buffer queue would
            // spin through callbacks: drop the samples and wait for as long as they would play.
            note("pcm-write-error", AAudio_convertResultToText(written), false);
            sleep_nanos(static_cast<std::int64_t>(count) * 1'000'000'000 / stream->sample_rate);
            written = static_cast<aaudio_result_t>(count);
        }
        done += static_cast<std::uint64_t>(written);
    }
    // A trailing partial frame cannot be played; report it consumed with a complete write.
    if (done == frames) return bytes;
    return static_cast<std::uint32_t>(done * frame_bytes);
}

bool AudioDriverBackend::pcm_start(std::uint64_t id) {
    std::shared_ptr<Pcm> stream = pcm(id);
    if (stream == nullptr) return false;
    std::lock_guard<std::mutex> lock(stream->lifecycle);
    stream->started = true;
    return stream->stream != nullptr && AAudioStream_requestStart(stream->stream) == AAUDIO_OK;
}

bool AudioDriverBackend::pcm_pause(std::uint64_t id) {
    std::shared_ptr<Pcm> stream = pcm(id);
    if (stream == nullptr) return false;
    stream->generation.fetch_add(1);
    std::lock_guard<std::mutex> lock(stream->lifecycle);
    stream->started = false;
    return stream->stream != nullptr && AAudioStream_requestPause(stream->stream) == AAUDIO_OK;
}

bool AudioDriverBackend::pcm_flush(std::uint64_t id) {
    std::shared_ptr<Pcm> stream = pcm(id);
    if (stream == nullptr) return false;
    stream->generation.fetch_add(1);
    std::lock_guard<std::mutex> lock(stream->lifecycle);
    stream->started = false;
    if (stream->stream == nullptr) return false;
    AAudioStream_requestPause(stream->stream);
    // Flushing is only valid once the pause has taken effect.
    aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
    AAudioStream_waitForStateChange(stream->stream, AAUDIO_STREAM_STATE_PAUSING, &next, 100'000'000);
    return AAudioStream_requestFlush(stream->stream) == AAUDIO_OK;
}

void AudioDriverBackend::pcm_gain(std::uint64_t id, float gain) {
    std::shared_ptr<Pcm> stream = pcm(id);
    if (stream != nullptr) stream->gain.store(std::clamp(gain, 0.0f, 1.0f));
}

void AudioDriverBackend::pcm_close(std::uint64_t id) {
    std::shared_ptr<Pcm> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pcms_.find(id);
        if (found == pcms_.end()) return;
        stream = std::move(found->second);
        pcms_.erase(found);
    }
    // A writer still inside pcm_write holds its own reference and stops at its next slice; the
    // AAudio stream closes when the last reference goes.
    stream->generation.fetch_add(1);
}

bool AudioDriverBackend::ensure_engine() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (engine_ != nullptr) return true;
    if (engine_tried_) return false;
    engine_tried_ = true;
    SLObjectItf object = nullptr;
    if (slCreateEngine(&object, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
        (*object)->Realize(object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        note("media-engine", "slCreateEngine failed");
        if (object != nullptr) (*object)->Destroy(object);
        return false;
    }
    SLEngineItf engine = nullptr;
    SLObjectItf mix = nullptr;
    if ((*object)->GetInterface(object, SL_IID_ENGINE, &engine) != SL_RESULT_SUCCESS ||
        (*engine)->CreateOutputMix(engine, &mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
        (*mix)->Realize(mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        note("media-engine", "output mix failed");
        if (mix != nullptr) (*mix)->Destroy(mix);
        (*object)->Destroy(object);
        return false;
    }
    engine_object_ = object;
    engine_ = engine;
    output_mix_ = mix;
    return true;
}

std::uint64_t AudioDriverBackend::media_open(const MediaSource& source) {
    if (!ensure_engine()) return 0;
    auto engine = static_cast<SLEngineItf>(engine_);
    SLDataLocator_AndroidFD fd_locator{SL_DATALOCATOR_ANDROIDFD, source.fd, source.offset,
                                       source.length < 0 ? SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE
                                                         : static_cast<SLAint64>(source.length)};
    SLDataLocator_URI uri_locator{SL_DATALOCATOR_URI,
                                  reinterpret_cast<SLchar*>(const_cast<char*>(source.uri.c_str()))};
    SLDataFormat_MIME mime{SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_UNSPECIFIED};
    SLDataSource data_source{source.is_fd ? static_cast<void*>(&fd_locator) : static_cast<void*>(&uri_locator),
                             &mime};
    SLDataLocator_OutputMix mix_locator{SL_DATALOCATOR_OUTPUTMIX, static_cast<SLObjectItf>(output_mix_)};
    SLDataSink sink{&mix_locator, nullptr};
    const SLInterfaceID ids[] = {SL_IID_SEEK, SL_IID_PREFETCHSTATUS, SL_IID_VOLUME};
    const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};

    auto media = std::make_shared<Media>();
    SLresult result = (*engine)->CreateAudioPlayer(engine, &media->player, &data_source, &sink, 3, ids, required);
    if (result == SL_RESULT_SUCCESS) result = (*media->player)->Realize(media->player, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        note("media-open-error", "result=" + std::to_string(result) +
                                     (source.is_fd ? " fd=" + std::to_string(source.fd) : " uri=" + source.uri));
        return 0;  // ~Media destroys a created but unrealized player
    }
    SLObjectItf player = media->player;
    if ((*player)->GetInterface(player, SL_IID_PLAY, &media->play) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_SEEK, &media->seek) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_VOLUME, &media->volume) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_PREFETCHSTATUS, &media->prefetch) != SL_RESULT_SUCCESS) {
        note("media-open-error", "missing interface");
        return 0;
    }
    (*media->play)->RegisterCallback(media->play, media_play_event, media.get());
    (*media->play)->SetCallbackEventsMask(media->play, SL_PLAYEVENT_HEADATEND);
    (*media->prefetch)->RegisterCallback(media->prefetch, media_prefetch_event, media.get());
    (*media->prefetch)->SetCallbackEventsMask(media->prefetch,
                                              SL_PREFETCHEVENT_STATUSCHANGE | SL_PREFETCHEVENT_FILLLEVELCHANGE);
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t id = next_id_++;
    medias_[id] = std::move(media);
    note("media-players", std::to_string(medias_.size()) + " open");
    return id;
}

bool AudioDriverBackend::media_play_state(std::uint64_t id, std::uint32_t state) {
    std::shared_ptr<Media> m = media(id);
    return m != nullptr && (*m->play)->SetPlayState(m->play, state) == SL_RESULT_SUCCESS;
}

bool AudioDriverBackend::media_seek(std::uint64_t id, std::uint32_t milliseconds) {
    std::shared_ptr<Media> m = media(id);
    return m != nullptr &&
           (*m->seek)->SetPosition(m->seek, milliseconds, SL_SEEKMODE_ACCURATE) == SL_RESULT_SUCCESS;
}

bool AudioDriverBackend::media_loop(std::uint64_t id, bool enabled) {
    std::shared_ptr<Media> m = media(id);
    return m != nullptr && (*m->seek)->SetLoop(m->seek, enabled ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE, 0,
                                               SL_TIME_UNKNOWN) == SL_RESULT_SUCCESS;
}

void AudioDriverBackend::media_gain(std::uint64_t id, float gain) {
    std::shared_ptr<Media> m = media(id);
    if (m != nullptr) (*m->volume)->SetVolumeLevel(m->volume, millibel(gain));
}

std::uint32_t AudioDriverBackend::media_position(std::uint64_t id) {
    std::shared_ptr<Media> m = media(id);
    SLmillisecond position = 0;
    if (m != nullptr) (*m->play)->GetPosition(m->play, &position);
    return position;
}

std::uint32_t AudioDriverBackend::media_duration(std::uint64_t id) {
    std::shared_ptr<Media> m = media(id);
    SLmillisecond duration = SL_TIME_UNKNOWN;
    if (m != nullptr) (*m->play)->GetDuration(m->play, &duration);
    return duration;
}

std::uint32_t AudioDriverBackend::media_status(std::uint64_t id) {
    std::shared_ptr<Media> m = media(id);
    if (m == nullptr) return ZB_AUDIO_STATUS_ERROR;
    return m->status.fetch_and(~ZB_AUDIO_STATUS_ENDED);
}

void AudioDriverBackend::media_close(std::uint64_t id) {
    std::shared_ptr<Media> m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = medias_.find(id);
        if (found == medias_.end()) return;
        m = std::move(found->second);
        medias_.erase(found);
    }
    // Released here, outside the map lock: Destroy waits for callbacks in flight.
}

}  // namespace zb
