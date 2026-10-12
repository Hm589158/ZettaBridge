#include "zb/host_gl.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gl/gl_diagnostics.h"
#include "zb/gl_hostcalls.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

HostGl::Call::Call(HostGl& host, GuestThread& thread, std::uint32_t index)
    : host_(host), thread_(thread), index_(index),
      regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]} {
    // Capture arguments before installing the default void/zero result.
    thread_.regs()[0] = 0;
    thread_.regs()[1] = 0;
}

std::uint32_t HostGl::Call::arg(unsigned position) {
    if (position < 4) return regs_[position];
    const std::uint64_t address = static_cast<std::uint64_t>(thread_.regs()[13]) + 4u * (position - 4u);
    if (address > UINT32_MAX) {
        fail(kGlInvalidValue, "argument address overflowed the guest stack");
        return 0;
    }
    const std::uint8_t* source =
        host_.runtime().memory().host_ptr(static_cast<std::uint32_t>(address), 4, kPageRead);
    if (source == nullptr) {
        fail(kGlInvalidValue, "argument is not on a readable guest stack");
        return 0;
    }
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

void HostGl::Call::fail(GLenum error, const char* reason) {
    if (!valid_) return;
    valid_ = false;
    host_.reject(*this, error, reason);
}

void HostGl::reject(Call& call, GLenum error, const char* reason) {
    const char* name = gl_host_call_name(call.index());
    log("GLES %s rejected: %s", name, reason);
    // Guests rarely call glGetError, so a rejection is otherwise silent (a texture that never
    // got its pixels renders black). Count them, and keep the first few with their arguments.
    static std::atomic<std::uint64_t> rejections{0};
    const std::uint64_t number = ++rejections;
    runtime_report().note_gl_detail("rejections", std::to_string(number), true);
    if (number <= 4) {
        char text[256];
        std::snprintf(text, sizeof text, "%s: %s args=0x%x,0x%x,0x%x,0x%x", name, reason, call.arg(0),
                      call.arg(1), call.arg(2), call.arg(3));
        runtime_report().note_gl_detail("rejection-" + std::to_string(number), text, false);
    }
    backend_.set_error(error);
}

#include "gen/gl_manual.inc"

#include "gen/gl_dispatch.inc"

namespace {

// The visibility diagnostics query GLES 2 state (the current program, framebuffer status,
// vertex attributes). A GLES 1 context has none of it, and each query would queue a
// GL_INVALID_ENUM that the guest's own glGetError then reads, so they skip GLES 1 contexts.
// Decided once per context per thread from GL_VERSION ("OpenGL ES-CM 1.1 ...").
bool gles2_context(HostGl& host) {
    thread_local std::uintptr_t t_context = 0;
    thread_local bool t_gles2 = true;
    const std::uintptr_t context = host.egl_context();
    if (context != t_context) {
        t_context = context;
        const GLubyte* version = host.backend().glGetString(0x1F02);  // GL_VERSION
        t_gles2 = version == nullptr ||
                  std::strncmp(reinterpret_cast<const char*>(version), "OpenGL ES-C", 11) != 0;
    }
    return t_gles2;
}

bool gl_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ZB_GL_TRACE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

}  // namespace

bool HostGl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (!is_gl_host_call(index)) return false;
    const char* name = gl_host_call_name(index);
    if (gl_trace_enabled()) {
        log("GLES trace: %s(0x%x, 0x%x, 0x%x, 0x%x)", name, thread.regs()[0], thread.regs()[1],
            thread.regs()[2], thread.regs()[3]);
    }
    const std::uint64_t host_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    runtime_report().note_gl_call(name, host_tid);
    const std::uint64_t egl_generation = runtime_report().egl_current_generation();
    if (egl_generation != gl_thread_sampled_generation_) {
        gl_thread_sampled_generation_ = egl_generation;
        runtime_report().note_gl_thread(host_tid);
    }
    // Probed once per GL thread: an engine may issue a few setup calls on its UI thread before
    // GLSurfaceView's GLThread has a context, so one sample from the first call alone cannot
    // tell a harmless early call from a renderer with no context at all.
    thread_local const HostGl* t_context_probed = nullptr;
    if (t_context_probed != this) {
        t_context_probed = this;
        if (egl_context_probe_) {
            const bool current = egl_context() != 0;
            if (!egl_context_checked_) {
                egl_context_checked_ = true;
                runtime_report().note_gl_egl_context(current);
            }
            runtime_report().note_gl_detail("context-tid-" + std::to_string(host_tid),
                                            std::string(current ? "yes" : "no") + " first=" + name,
                                            false);
        }
    }
    Call call(*this, thread, index);
    const bool diagnose = gl_diagnostics_enabled() && gles2_context(*this);
    if (diagnose) gl_diagnose_before(*this, call);
    if (dispatch(call)) {
        // Visibility diagnostics query the driver, so they run only once the Android runtime
        // enables them; mock-backend tests keep exact call logs.
        if (call.valid() && diagnose) gl_diagnose(*this, call);
        if (call.valid() && std::strcmp(name, "glGetError") == 0) {
            const std::uint32_t error = thread.regs()[0];
            if (error != 0) runtime_report().note_gl_error(name, error);
        }
        return true;
    }
    log("GLES host call index %u has no generated handler", index);
    std::abort();
}

}  // namespace zb
