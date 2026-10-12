#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "zb/audio_backend.h"

namespace zb {

// The device side of guest audio. PCM from guest buffer queue players goes to one AAudio output
// stream per player; compressed sources (an OpenSL ES URI or Android file descriptor) go to a
// player of the platform's own 64-bit OpenSL ES, which decodes them. Streams and players live in
// shared_ptrs, so a close that races a blocking write or a query only drops the map entry and the
// last user releases the device object.
class AudioDriverBackend final : public AudioBackend {
public:
    AudioDriverBackend() = default;
    ~AudioDriverBackend() override;
    AudioDriverBackend(const AudioDriverBackend&) = delete;
    AudioDriverBackend& operator=(const AudioDriverBackend&) = delete;

    std::uint64_t pcm_open(std::uint32_t sample_rate, std::uint32_t channels, Encoding encoding) override;
    std::uint32_t pcm_write(std::uint64_t stream, const void* data, std::uint32_t bytes) override;
    bool pcm_start(std::uint64_t stream) override;
    bool pcm_pause(std::uint64_t stream) override;
    bool pcm_flush(std::uint64_t stream) override;
    void pcm_gain(std::uint64_t stream, float gain) override;
    void pcm_close(std::uint64_t stream) override;

    std::uint64_t media_open(const MediaSource& source) override;
    bool media_play_state(std::uint64_t media, std::uint32_t state) override;
    bool media_seek(std::uint64_t media, std::uint32_t milliseconds) override;
    bool media_loop(std::uint64_t media, bool enabled) override;
    void media_gain(std::uint64_t media, float gain) override;
    std::uint32_t media_position(std::uint64_t media) override;
    std::uint32_t media_duration(std::uint64_t media) override;
    std::uint32_t media_status(std::uint64_t media) override;
    void media_close(std::uint64_t media) override;

    struct Pcm;
    struct Media;

private:
    std::shared_ptr<Pcm> pcm(std::uint64_t id);
    std::shared_ptr<Media> media(std::uint64_t id);
    bool ensure_engine();

    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Pcm>> pcms_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Media>> medias_;
    std::uint64_t next_id_ = 1;

    // The platform OpenSL ES engine and output mix, created on the first compressed source.
    std::mutex engine_mutex_;
    bool engine_tried_ = false;
    const void* engine_object_ = nullptr;  // SLObjectItf
    const void* engine_ = nullptr;         // SLEngineItf
    const void* output_mix_ = nullptr;     // SLObjectItf
};

}  // namespace zb
