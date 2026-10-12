// HostAudio: the audio host calls of the guest libOpenSLES.so reach the AudioBackend with guest
// pointers translated and bounds-checked, handles stay 32-bit, and with no backend installed
// the range is left unserved so the guest falls back to silence.
#include <sys/mman.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/audio_protocol.h"
#include "zb/host_audio.h"

namespace {

constexpr std::uint32_t kData = 0x20000;

class MockAudio final : public zb::AudioBackend {
public:
    std::vector<std::string> log;
    std::uint32_t last_rate = 0, last_channels = 0;
    Encoding last_encoding = Encoding::kS16;
    const void* last_data = nullptr;
    std::uint32_t last_bytes = 0;
    float last_gain = -1.0f;
    MediaSource last_media;

    std::uint64_t pcm_open(std::uint32_t rate, std::uint32_t channels, Encoding encoding) override {
        last_rate = rate;
        last_channels = channels;
        last_encoding = encoding;
        log.push_back("pcm_open");
        return 0x1234567890ull;  // a 64-bit id that must not reach the guest
    }
    std::uint32_t pcm_write(std::uint64_t, const void* data, std::uint32_t bytes) override {
        last_data = data;
        last_bytes = bytes;
        log.push_back("pcm_write");
        return bytes;
    }
    bool pcm_start(std::uint64_t) override { log.push_back("pcm_start"); return true; }
    bool pcm_pause(std::uint64_t) override { log.push_back("pcm_pause"); return true; }
    bool pcm_flush(std::uint64_t) override { log.push_back("pcm_flush"); return true; }
    void pcm_gain(std::uint64_t, float gain) override { last_gain = gain; log.push_back("pcm_gain"); }
    void pcm_close(std::uint64_t) override { log.push_back("pcm_close"); }

    std::uint64_t media_open(const MediaSource& source) override {
        last_media = source;
        log.push_back("media_open");
        return 77;
    }
    bool media_play_state(std::uint64_t, std::uint32_t) override { log.push_back("media_play_state"); return true; }
    bool media_seek(std::uint64_t, std::uint32_t) override { log.push_back("media_seek"); return true; }
    bool media_loop(std::uint64_t, bool) override { log.push_back("media_loop"); return true; }
    void media_gain(std::uint64_t, float) override { log.push_back("media_gain"); }
    std::uint32_t media_position(std::uint64_t) override { return 1500; }
    std::uint32_t media_duration(std::uint64_t) override { return 90000; }
    std::uint32_t media_status(std::uint64_t) override { return ZB_AUDIO_STATUS_PREFETCHED; }
    void media_close(std::uint64_t) override { log.push_back("media_close"); }
};

std::uint32_t call(zb::HostAudio& audio, zb::GuestThread& thread, std::uint32_t index, std::uint32_t a0 = 0,
                   std::uint32_t a1 = 0, std::uint32_t a2 = 0) {
    thread.regs()[0] = a0;
    thread.regs()[1] = a1;
    thread.regs()[2] = a2;
    CHECK(audio.handle_host_call(index, thread));
    return thread.regs()[0];
}

}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    zb::HostAudio audio(runtime);

    // No backend: the whole range is left to the runtime's "unimplemented" answer of 0.
    CHECK(!audio.handle_host_call(ZB_AUDIO_HC_PROBE, thread));
    CHECK(!audio.handle_host_call(0xF9FF, thread));

    MockAudio backend;
    audio.set_backend(&backend);
    CHECK(call(audio, thread, ZB_AUDIO_HC_PROBE) == ZB_AUDIO_PROBE_MAGIC);
    CHECK(!audio.handle_host_call(ZB_AUDIO_HOST_CALL_LAST + 1, thread));

    // PCM: a small handle, the guest buffer translated to its host address.
    const std::uint32_t pcm = call(audio, thread, ZB_AUDIO_HC_PCM_OPEN, 44100, 2, ZB_AUDIO_ENCODING_S16);
    CHECK(pcm != 0 && pcm < 0x10000);
    CHECK(backend.last_rate == 44100 && backend.last_channels == 2 &&
          backend.last_encoding == zb::AudioBackend::Encoding::kS16);
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_WRITE, pcm, kData + 0x100, 512) == 512);
    CHECK(backend.last_data == runtime.memory().base() + kData + 0x100 && backend.last_bytes == 512);
    // A buffer running past mapped memory never reaches the backend.
    backend.log.clear();
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_WRITE, pcm, kData + 0xF00, 0x200) == 0);
    CHECK(backend.log.empty());
    const float half = 0.5f;
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_CONTROL, pcm, ZB_AUDIO_OP_GAIN, std::bit_cast<std::uint32_t>(half)) == 0);
    CHECK(backend.last_gain == 0.5f);
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_CONTROL, pcm, ZB_AUDIO_OP_START) == 0);
    CHECK(backend.log.back() == "pcm_start");
    // Unknown encodings and handles are refused.
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_OPEN, 44100, 2, 9) == 0);
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_WRITE, pcm + 100, kData, 4) == 0);
    // A PCM handle is not a media handle.
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_QUERY, pcm, ZB_AUDIO_QUERY_STATUS) == ZB_AUDIO_STATUS_ERROR);
    call(audio, thread, ZB_AUDIO_HC_PCM_CLOSE, pcm);
    CHECK(backend.log.back() == "pcm_close");
    CHECK(call(audio, thread, ZB_AUDIO_HC_PCM_WRITE, pcm, kData, 4) == 0);

    // Media from a file descriptor, with a 64-bit offset and "to the end" length.
    zb_audio_media fd_source{ZB_AUDIO_MEDIA_FD, 42, 0x10, 0x1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0};
    std::memcpy(runtime.memory().base() + kData + 0x200, &fd_source, sizeof fd_source);
    const std::uint32_t media = call(audio, thread, ZB_AUDIO_HC_MEDIA_OPEN, kData + 0x200);
    CHECK(media != 0);
    CHECK(backend.last_media.is_fd && backend.last_media.fd == 42 &&
          backend.last_media.offset == 0x100000010ll && backend.last_media.length == -1);
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_QUERY, media, ZB_AUDIO_QUERY_POSITION) == 1500);
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_QUERY, media, ZB_AUDIO_QUERY_DURATION) == 90000);
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_QUERY, media, ZB_AUDIO_QUERY_STATUS) == ZB_AUDIO_STATUS_PREFETCHED);
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_CONTROL, media, ZB_AUDIO_OP_PLAY_STATE, 3) == 0);
    CHECK(backend.log.back() == "media_play_state");
    call(audio, thread, ZB_AUDIO_HC_MEDIA_CLOSE, media);
    CHECK(backend.log.back() == "media_close");

    // Media from a URI string in guest memory.
    const char uri[] = "file:///sdcard/music.ogg";
    std::memcpy(runtime.memory().base() + kData + 0x300, uri, sizeof uri);
    zb_audio_media uri_source{ZB_AUDIO_MEDIA_URI, -1, 0, 0, 0, 0, kData + 0x300};
    std::memcpy(runtime.memory().base() + kData + 0x200, &uri_source, sizeof uri_source);
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_OPEN, kData + 0x200) != 0);
    CHECK(!backend.last_media.is_fd && backend.last_media.uri == uri);
    // An unterminated URI that runs off mapped memory is refused.
    std::memset(runtime.memory().base() + kData + 0xF00, 'a', 0x100);
    uri_source.uri = kData + 0xF00;
    std::memcpy(runtime.memory().base() + kData + 0x200, &uri_source, sizeof uri_source);
    CHECK(call(audio, thread, ZB_AUDIO_HC_MEDIA_OPEN, kData + 0x200) == 0);

    std::puts("audio_test PASS");
    return 0;
}
