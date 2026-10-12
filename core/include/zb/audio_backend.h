#pragma once

#include <cstdint>
#include <string>

namespace zb {

// The host side of guest audio (core/include/zb/audio_protocol.h). Android implements it with
// AAudio for PCM and the platform OpenSL ES for compressed media
// (core/android/audio_driver_backend.cpp); host tests use a mock. Ids are the backend's own and
// never reach the guest: HostAudio hands out 32-bit handles.
class AudioBackend {
public:
    enum class Encoding { kU8, kS16, kFloat };

    struct MediaSource {
        bool is_fd = false;
        int fd = -1;
        std::int64_t offset = 0;
        std::int64_t length = -1;  // -1: to the end of the file
        std::string uri;
    };

    virtual ~AudioBackend() = default;

    // 0 on failure.
    virtual std::uint64_t pcm_open(std::uint32_t sample_rate, std::uint32_t channels, Encoding encoding) = 0;
    // Plays the bytes and blocks until the device has taken them, or returns early with the number
    // taken once the stream is paused, flushed or closed from another thread.
    virtual std::uint32_t pcm_write(std::uint64_t stream, const void* data, std::uint32_t bytes) = 0;
    virtual bool pcm_start(std::uint64_t stream) = 0;
    virtual bool pcm_pause(std::uint64_t stream) = 0;
    virtual bool pcm_flush(std::uint64_t stream) = 0;
    virtual void pcm_gain(std::uint64_t stream, float gain) = 0;
    virtual void pcm_close(std::uint64_t stream) = 0;

    // 0 on failure.
    virtual std::uint64_t media_open(const MediaSource& source) = 0;
    // SL_PLAYSTATE_* values: 1 stopped, 2 paused, 3 playing.
    virtual bool media_play_state(std::uint64_t media, std::uint32_t state) = 0;
    virtual bool media_seek(std::uint64_t media, std::uint32_t milliseconds) = 0;
    virtual bool media_loop(std::uint64_t media, bool enabled) = 0;
    virtual void media_gain(std::uint64_t media, float gain) = 0;
    virtual std::uint32_t media_position(std::uint64_t media) = 0;
    // 0xFFFFFFFF while unknown.
    virtual std::uint32_t media_duration(std::uint64_t media) = 0;
    // ZB_AUDIO_STATUS_* bits; ENDED is reported once per end.
    virtual std::uint32_t media_status(std::uint64_t media) = 0;
    virtual void media_close(std::uint64_t media) = 0;
};

}  // namespace zb
