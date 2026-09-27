// Luồng gốc trên Linux và Android (bionic): pthread cho tạo và join, tên đặt bằng
// pthread_setname_np và đọc bằng prctl vì pthread_getname_np chỉ có từ Android API 26.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <linux/prctl.h>
#include <pthread.h>
#include <sys/prctl.h>

#include <array>
#include <bit>
#include <cstring>

namespace orion::jobs::detail {
namespace {

// pthread_t có từ <pthread.h>; clang-tidy 20 đòi header nội bộ <bits/pthreadtypes.h> của glibc.
// NOLINTNEXTLINE(misc-include-cleaner)
using PosixThread = pthread_t;

void* thread_entry(void* argument) {
    static_cast<ThreadState*>(argument)->enter();
    return nullptr;
}

}  // namespace

Result<NativeThread> start_native_thread(ThreadState& state) noexcept {
    static_assert(sizeof(PosixThread) == sizeof(u64));
    PosixThread thread{};
    const int error = pthread_create(&thread, nullptr, &thread_entry, &state);
    if (error != 0) {
        return fail(ErrorCode::ResourceExhausted, "jobs: pthread_create thất bại", error);
    }
    return NativeThread{std::bit_cast<u64>(thread)};
}

void join_native_thread(const NativeThread thread) noexcept {
    const int error = pthread_join(std::bit_cast<PosixThread>(thread.handle), nullptr);
    ORION_VERIFY(error == 0, "pthread_join trả {}", error);
}

void set_current_thread_name(const char* name) noexcept {
    // Tên đã được kiểm dài tối đa 15 byte nên lời gọi không thể thất bại vì ERANGE.
    static_cast<void>(pthread_setname_np(pthread_self(), name));
}

usize current_thread_name(std::array<char, kMaxThreadName + 1>& buffer) noexcept {
    buffer.fill('\0');
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): prctl là API variadic duy nhất có mọi bản.
    if (prctl(PR_GET_NAME, buffer.data(), 0UL, 0UL, 0UL) != 0) {
        return 0;
    }
    buffer.back() = '\0';
    return std::strlen(buffer.data());
}

}  // namespace orion::jobs::detail
