#include "zb/host_audio.h"

#include <bit>
#include <cstring>
#include <string>

#include "zb/audio_protocol.h"
#include "zb/log.h"

namespace zb {

namespace {

// Longest URI accepted from the guest; OpenSL ES URIs are paths or http URLs.
constexpr std::uint32_t kMaxUri = 4096;

void answer(GuestThread& thread, std::uint32_t value) {
    thread.regs()[0] = value;
    thread.regs()[1] = 0;
}

}  // namespace

std::uint32_t HostAudio::add(Kind kind, std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t handle = next_handle_++;
    if (handle == 0) handle = next_handle_++;
    entries_[handle] = Entry{kind, id};
    return handle;
}

std::uint64_t HostAudio::find(std::uint32_t handle, Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(handle);
    return found == entries_.end() || found->second.kind != kind ? 0 : found->second.id;
}

std::uint64_t HostAudio::remove(std::uint32_t handle, Kind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(handle);
    if (found == entries_.end() || found->second.kind != kind) return 0;
    const std::uint64_t id = found->second.id;
    entries_.erase(found);
    return id;
}

std::int32_t HostAudio::control(Kind kind, std::uint64_t id, std::uint32_t op, std::uint32_t argument) {
    AudioBackend* backend = backend_.load(std::memory_order_acquire);
    if (kind == Kind::kPcm) {
        switch (op) {
        case ZB_AUDIO_OP_START: return backend->pcm_start(id) ? 0 : -1;
        case ZB_AUDIO_OP_PAUSE: return backend->pcm_pause(id) ? 0 : -1;
        case ZB_AUDIO_OP_FLUSH: return backend->pcm_flush(id) ? 0 : -1;
        case ZB_AUDIO_OP_GAIN: backend->pcm_gain(id, std::bit_cast<float>(argument)); return 0;
        default: return -1;
        }
    }
    switch (op) {
    case ZB_AUDIO_OP_PLAY_STATE: return backend->media_play_state(id, argument) ? 0 : -1;
    case ZB_AUDIO_OP_SEEK: return backend->media_seek(id, argument) ? 0 : -1;
    case ZB_AUDIO_OP_LOOP: return backend->media_loop(id, argument != 0) ? 0 : -1;
    case ZB_AUDIO_OP_GAIN: backend->media_gain(id, std::bit_cast<float>(argument)); return 0;
    default: return -1;
    }
}

std::uint32_t HostAudio::open_media(std::uint32_t guest_description) {
    const std::uint8_t* raw = runtime_.memory().host_ptr(guest_description, sizeof(zb_audio_media), kPageRead);
    if (raw == nullptr) return 0;
    zb_audio_media description;
    std::memcpy(&description, raw, sizeof description);
    AudioBackend::MediaSource source;
    if (description.kind == ZB_AUDIO_MEDIA_FD) {
        source.is_fd = true;
        source.fd = description.fd;
        source.offset = static_cast<std::int64_t>((static_cast<std::uint64_t>(description.offset_hi) << 32) |
                                                  description.offset_lo);
        const std::uint64_t length = (static_cast<std::uint64_t>(description.length_hi) << 32) | description.length_lo;
        source.length = length == UINT64_MAX ? -1 : static_cast<std::int64_t>(length);
    } else if (description.kind == ZB_AUDIO_MEDIA_URI) {
        // Read the string a page-bounded chunk at a time: it may end just before unmapped memory.
        for (std::uint32_t i = 0; i < kMaxUri; ++i) {
            const std::uint8_t* c = runtime_.memory().host_ptr(description.uri + i, 1, kPageRead);
            if (c == nullptr) return 0;
            if (*c == 0) break;
            source.uri.push_back(static_cast<char>(*c));
        }
        if (source.uri.empty() || source.uri.size() >= kMaxUri) return 0;
    } else {
        return 0;
    }
    AudioBackend* backend = backend_.load(std::memory_order_acquire);
    const std::uint64_t id = backend->media_open(source);
    if (id == 0) {
        log("audio: cannot open %s", source.is_fd ? "an fd source" : source.uri.c_str());
        return 0;
    }
    return add(Kind::kMedia, id);
}

bool HostAudio::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_AUDIO_HOST_CALL_FIRST || index > ZB_AUDIO_HOST_CALL_LAST) return false;
    AudioBackend* backend = backend_.load(std::memory_order_acquire);
    if (backend == nullptr) return false;
    const std::uint32_t a0 = thread.regs()[0];
    const std::uint32_t a1 = thread.regs()[1];
    const std::uint32_t a2 = thread.regs()[2];
    switch (index) {
    case ZB_AUDIO_HC_PROBE:
        answer(thread, ZB_AUDIO_PROBE_MAGIC);
        return true;
    case ZB_AUDIO_HC_PCM_OPEN: {
        AudioBackend::Encoding encoding;
        switch (a2) {
        case ZB_AUDIO_ENCODING_U8: encoding = AudioBackend::Encoding::kU8; break;
        case ZB_AUDIO_ENCODING_S16: encoding = AudioBackend::Encoding::kS16; break;
        case ZB_AUDIO_ENCODING_FLOAT: encoding = AudioBackend::Encoding::kFloat; break;
        default: answer(thread, 0); return true;
        }
        if (a0 == 0 || a0 > 384000 || a1 == 0 || a1 > 8) {
            answer(thread, 0);
            return true;
        }
        const std::uint64_t id = backend->pcm_open(a0, a1, encoding);
        answer(thread, id == 0 ? 0 : add(Kind::kPcm, id));
        return true;
    }
    case ZB_AUDIO_HC_PCM_WRITE: {
        const std::uint64_t id = find(a0, Kind::kPcm);
        const std::uint8_t* data = a2 == 0 ? nullptr : runtime_.memory().host_ptr(a1, a2, kPageRead);
        if (id == 0 || data == nullptr) {
            answer(thread, 0);
            return true;
        }
        // Blocks this guest thread's host thread until the device took the samples; that is the
        // pacing a buffer queue player needs.
        answer(thread, backend->pcm_write(id, data, a2));
        return true;
    }
    case ZB_AUDIO_HC_PCM_CONTROL:
    case ZB_AUDIO_HC_MEDIA_CONTROL: {
        const Kind kind = index == ZB_AUDIO_HC_PCM_CONTROL ? Kind::kPcm : Kind::kMedia;
        const std::uint64_t id = find(a0, kind);
        answer(thread, id == 0 ? static_cast<std::uint32_t>(-1) : static_cast<std::uint32_t>(control(kind, id, a1, a2)));
        return true;
    }
    case ZB_AUDIO_HC_PCM_CLOSE: {
        const std::uint64_t id = remove(a0, Kind::kPcm);
        if (id != 0) backend->pcm_close(id);
        answer(thread, 0);
        return true;
    }
    case ZB_AUDIO_HC_MEDIA_OPEN:
        answer(thread, open_media(a0));
        return true;
    case ZB_AUDIO_HC_MEDIA_QUERY: {
        const std::uint64_t id = find(a0, Kind::kMedia);
        std::uint32_t value = 0;
        if (id == 0) {
            value = a1 == ZB_AUDIO_QUERY_STATUS ? ZB_AUDIO_STATUS_ERROR : 0;
        } else if (a1 == ZB_AUDIO_QUERY_POSITION) {
            value = backend->media_position(id);
        } else if (a1 == ZB_AUDIO_QUERY_DURATION) {
            value = backend->media_duration(id);
        } else if (a1 == ZB_AUDIO_QUERY_STATUS) {
            value = backend->media_status(id);
        }
        answer(thread, value);
        return true;
    }
    case ZB_AUDIO_HC_MEDIA_CLOSE: {
        const std::uint64_t id = remove(a0, Kind::kMedia);
        if (id != 0) backend->media_close(id);
        answer(thread, 0);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace zb
