#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "zb/audio_backend.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"

namespace zb {

// Audio host-call dispatcher (ZB_AUDIO_HC_*, core/include/zb/audio_protocol.h) for the guest
// libOpenSLES.so. The guest keeps the whole OpenSL ES object model; the host only plays: PCM that
// buffer queue players produce, and compressed sources (URI / file descriptor) it decodes itself.
//
// The backend is installed after construction (set_backend) because only the Android build has
// one. Without it PROBE goes unanswered and the guest library stays silent.
class HostAudio {
public:
    explicit HostAudio(LibraryRuntime& runtime) : runtime_(runtime) {}
    HostAudio(const HostAudio&) = delete;
    HostAudio& operator=(const HostAudio&) = delete;

    void set_backend(AudioBackend* backend) { backend_.store(backend, std::memory_order_release); }

    // Serves the 0xFA00-0xFAFF range; false for any other index, and for the whole range while
    // there is no backend (the runtime then reports it as unimplemented and answers 0).
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    enum class Kind { kPcm, kMedia };
    struct Entry {
        Kind kind;
        std::uint64_t id;
    };

    std::uint32_t add(Kind kind, std::uint64_t id);
    // The backend id of a live handle of that kind, or 0.
    std::uint64_t find(std::uint32_t handle, Kind kind);
    std::uint64_t remove(std::uint32_t handle, Kind kind);

    std::int32_t control(Kind kind, std::uint64_t id, std::uint32_t op, std::uint32_t argument);
    std::uint32_t open_media(std::uint32_t guest_description);

    LibraryRuntime& runtime_;
    std::atomic<AudioBackend*> backend_{nullptr};
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::uint32_t next_handle_ = 1;
};

}  // namespace zb
