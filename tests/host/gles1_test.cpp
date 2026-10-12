// GLES 1.1 marshaling: the fixed-function client arrays (glVertexPointer and friends) follow the
// glVertexAttribPointer rules, and the pointer-taking state calls read the right element counts.
#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_gles.h"
#include "zb/gl_hostcalls.h"
#include "zb/host_gl.h"

namespace {

constexpr std::uint32_t kStack = 0x10000;
constexpr std::uint32_t kData = 0x20000;

constexpr std::uint32_t kFloat = 0x1406;
constexpr std::uint32_t kUnsignedByte = 0x1401;
constexpr std::uint32_t kVertexArray = 0x8074;
constexpr std::uint32_t kColorArray = 0x8076;
constexpr std::uint32_t kTextureCoordArray = 0x8078;
constexpr std::uint32_t kTexture0 = 0x84C0;
constexpr std::uint32_t kArrayBuffer = 0x8892;

void call(zb::HostGl& host, zb::LibraryRuntime& runtime, zb::GuestThread& thread,
          std::uint32_t index, const std::array<std::uint32_t, 8>& words) {
    for (unsigned i = 0; i < 4; ++i) thread.regs()[i] = words[i];
    thread.regs()[13] = kStack;
    std::memcpy(runtime.memory().base() + kStack, words.data() + 4,
                (words.size() - 4) * sizeof(words[0]));
    CHECK(host.handle_host_call(index, thread));
}

const MockGles::Call* last(const MockGles& backend, const char* name) {
    for (auto it = backend.calls().rbegin(); it != backend.calls().rend(); ++it) {
        if (it->name == name) return &*it;
    }
    return nullptr;
}

std::size_t position(const MockGles& backend, const char* name, std::size_t from = 0) {
    for (std::size_t i = from; i < backend.calls().size(); ++i) {
        if (backend.calls()[i].name == name) return i;
    }
    return SIZE_MAX;
}

}  // namespace

int main() {
    CHECK(zb::kGlHostCall1Count == 87);
    CHECK(zb::kGlHostCall1First == 492 && zb::kGlHostCall1Last == 578);
    CHECK(std::strcmp(zb::gl_host_call_name(zb::ZB_GL_HC_glMatrixMode), "glMatrixMode") == 0);
    CHECK(zb::gl_host_call(zb::kGlHostCall1Last + 1) == nullptr);

    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kStack, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(kData, 0x2000, PROT_READ | PROT_WRITE));
    MockGles backend;
    zb::HostGl host(runtime, backend);
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    const auto host_address = [&](std::uint32_t guest) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(runtime.memory().base() + guest));
    };

    // Scalar fixed-function calls go straight through (the calls the Pocket Army report named).
    call(host, runtime, thread, zb::ZB_GL_HC_glMatrixMode, {0x1701});
    call(host, runtime, thread, zb::ZB_GL_HC_glLoadIdentity, {});
    const float half = 0.5f;
    std::uint32_t half_bits;
    std::memcpy(&half_bits, &half, sizeof half_bits);
    call(host, runtime, thread, zb::ZB_GL_HC_glAlphaFunc, {0x0204, half_bits});
    const MockGles::Call* alpha = last(backend, "glAlphaFunc");
    CHECK(alpha != nullptr && alpha->arguments[0] == 0x0204 && alpha->arguments[1] == half_bits);
    CHECK(last(backend, "glMatrixMode") != nullptr && last(backend, "glLoadIdentity") != nullptr);

    // glOrthof takes six floats: two of them come from the guest stack.
    call(host, runtime, thread, zb::ZB_GL_HC_glOrthof, {1, 2, 3, 4, 5, 6});
    const MockGles::Call* ortho = last(backend, "glOrthof");
    CHECK(ortho != nullptr && ortho->arguments.size() == 6 && ortho->arguments[4] == 5 &&
          ortho->arguments[5] == 6);

    // A matrix is 16 floats read from guest memory.
    call(host, runtime, thread, zb::ZB_GL_HC_glLoadMatrixf, {kData + 0x400});
    const MockGles::Call* matrix = last(backend, "glLoadMatrixf");
    CHECK(matrix != nullptr && matrix->arguments[0] == host_address(kData + 0x400));
    backend.set_error(0);
    backend.clear_calls();
    call(host, runtime, thread, zb::ZB_GL_HC_glLoadMatrixf, {kData + 0x2000 - 32});
    CHECK(last(backend, "glLoadMatrixf") == nullptr);
    CHECK(backend.error() == zb::kGlInvalidValue);

    // GL_POSITION is four floats, GL_SPOT_EXPONENT one: the last float of the mapping only fits
    // the one-element parameter.
    backend.set_error(0);
    call(host, runtime, thread, zb::ZB_GL_HC_glLightfv, {0x4000, 0x1203, kData + 0x2000 - 4});
    CHECK(last(backend, "glLightfv") == nullptr && backend.error() == zb::kGlInvalidValue);
    call(host, runtime, thread, zb::ZB_GL_HC_glLightfv, {0x4000, 0x1205, kData + 0x2000 - 4});
    CHECK(last(backend, "glLightfv") != nullptr);

    // Client vertex and color arrays: recorded, then handed over at draw time as host pointers.
    call(host, runtime, thread, zb::ZB_GL_HC_glBindBuffer, {kArrayBuffer, 0});
    backend.clear_calls();
    call(host, runtime, thread, zb::ZB_GL_HC_glVertexPointer, {2, kFloat, 0, kData + 0x100});
    call(host, runtime, thread, zb::ZB_GL_HC_glColorPointer, {4, kUnsignedByte, 0, kData + 0x200});
    CHECK(last(backend, "glVertexPointer") == nullptr && last(backend, "glColorPointer") == nullptr);
    call(host, runtime, thread, zb::ZB_GL_HC_glEnableClientState, {kVertexArray});
    call(host, runtime, thread, zb::ZB_GL_HC_glEnableClientState, {kColorArray});
    backend.clear_calls();
    call(host, runtime, thread, zb::ZB_GL_HC_glDrawArrays, {4, 0, 3});
    const MockGles::Call* vertex = last(backend, "glVertexPointer");
    CHECK(vertex != nullptr && vertex->arguments[0] == 2 && vertex->arguments[1] == kFloat &&
          vertex->arguments[3] == host_address(kData + 0x100));
    const MockGles::Call* color = last(backend, "glColorPointer");
    CHECK(color != nullptr && color->arguments[3] == host_address(kData + 0x200));
    CHECK(position(backend, "glVertexPointer") < position(backend, "glDrawArrays"));

    // A disabled array is not materialized.
    call(host, runtime, thread, zb::ZB_GL_HC_glDisableClientState, {kColorArray});
    backend.clear_calls();
    call(host, runtime, thread, zb::ZB_GL_HC_glDrawArrays, {4, 0, 3});
    CHECK(last(backend, "glColorPointer") == nullptr && last(backend, "glVertexPointer") != nullptr);

    // Texture coordinates belong to the client-active unit; materializing unit 1 while unit 0 is
    // active selects unit 1 for the call and restores unit 0 after it.
    call(host, runtime, thread, zb::ZB_GL_HC_glClientActiveTexture, {kTexture0 + 1});
    call(host, runtime, thread, zb::ZB_GL_HC_glTexCoordPointer, {2, kFloat, 16, kData + 0x300});
    call(host, runtime, thread, zb::ZB_GL_HC_glEnableClientState, {kTextureCoordArray});
    call(host, runtime, thread, zb::ZB_GL_HC_glClientActiveTexture, {kTexture0});
    backend.clear_calls();
    call(host, runtime, thread, zb::ZB_GL_HC_glDrawArrays, {4, 0, 3});
    const std::size_t select = position(backend, "glClientActiveTexture");
    const std::size_t texcoord = position(backend, "glTexCoordPointer");
    const std::size_t restore = position(backend, "glClientActiveTexture", select + 1);
    CHECK(select != SIZE_MAX && texcoord != SIZE_MAX && restore != SIZE_MAX);
    CHECK(select < texcoord && texcoord < restore);
    CHECK(backend.calls()[select].arguments[0] == kTexture0 + 1);
    CHECK(backend.calls()[restore].arguments[0] == kTexture0);
    CHECK(backend.calls()[texcoord].arguments[3] == host_address(kData + 0x300));

    // glGetPointerv answers with the guest address the guest passed.
    call(host, runtime, thread, zb::ZB_GL_HC_glGetPointerv, {0x808E, kData + 0x80});
    std::uint32_t queried = 0;
    std::memcpy(&queried, runtime.memory().base() + kData + 0x80, sizeof queried);
    CHECK(queried == kData + 0x100);

    // With a buffer bound the pointer is an offset and reaches the driver immediately.
    call(host, runtime, thread, zb::ZB_GL_HC_glBindBuffer, {kArrayBuffer, 7});
    backend.clear_calls();
    call(host, runtime, thread, zb::ZB_GL_HC_glVertexPointer, {3, kFloat, 12, 24});
    vertex = last(backend, "glVertexPointer");
    CHECK(vertex != nullptr && vertex->arguments[3] == 24);
    call(host, runtime, thread, zb::ZB_GL_HC_glBindBuffer, {kArrayBuffer, 0});

    // A client array whose draw range runs past mapped guest memory drops the draw.
    call(host, runtime, thread, zb::ZB_GL_HC_glVertexPointer, {4, kFloat, 0, kData + 0x2000 - 16});
    backend.clear_calls();
    backend.set_error(0);
    call(host, runtime, thread, zb::ZB_GL_HC_glDrawArrays, {4, 0, 2});
    CHECK(last(backend, "glDrawArrays") == nullptr);
    CHECK(backend.error() == zb::kGlInvalidOperation);

    std::puts("gles1_test PASS");
    return 0;
}
