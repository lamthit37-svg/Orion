// Luồng gốc trên iOS và macOS: pthread; Apple chỉ cho đặt tên luồng đang chạy.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <pthread.h>

#include <array>
#include <bit>
#include <cstring>

namespace orion::jobs::detail {
namespace {

void* thread_entry(void* argument) {
    static_cast<ThreadState*>(argument)->enter();
    return nullptr;
}

}  // namespace

Result<NativeThread> start_native_thread(ThreadState& state) noexcept {
    static_assert(sizeof(pthread_t) == sizeof(u64));
    pthread_t thread{};
    const int error = pthread_create(&thread, nullptr, &thread_entry, &state);
    if (error != 0) {
        return fail(ErrorCode::ResourceExhausted, "jobs: pthread_create thất bại", error);
    }
    return NativeThread{std::bit_cast<u64>(thread)};
}

void join_native_thread(const NativeThread thread) noexcept {
    const int error = pthread_join(std::bit_cast<pthread_t>(thread.handle), nullptr);
    ORION_VERIFY(error == 0, "pthread_join trả {}", error);
}

void set_current_thread_name(const char* name) noexcept {
    static_cast<void>(pthread_setname_np(name));
}

usize current_thread_name(std::array<char, kMaxThreadName + 1>& buffer) noexcept {
    buffer.fill('\0');
    if (pthread_getname_np(pthread_self(), buffer.data(), buffer.size()) != 0) {
        return 0;
    }
    buffer.back() = '\0';
    return std::strlen(buffer.data());
}

}  // namespace orion::jobs::detail
