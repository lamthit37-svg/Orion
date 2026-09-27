// Luồng gốc trên Windows: _beginthreadex (để CRT khởi tạo đúng cho luồng mới), tên luồng bằng
// SetThreadDescription (Windows 10 1607 trở lên), hiện trong debugger và profiler.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <process.h>
#include <windows.h>

#include <array>
#include <bit>
#include <cerrno>

namespace orion::jobs::detail {
namespace {

unsigned __stdcall thread_entry(void* argument) {
    static_cast<ThreadState*>(argument)->enter();
    return 0;
}

}  // namespace

Result<NativeThread> start_native_thread(ThreadState& state) noexcept {
    const uintptr_t handle = _beginthreadex(nullptr, 0, &thread_entry, &state, 0, nullptr);
    if (handle == 0) {
        return fail(ErrorCode::ResourceExhausted, "jobs: _beginthreadex thất bại", errno);
    }
    return NativeThread{static_cast<u64>(handle)};
}

void join_native_thread(const NativeThread thread) noexcept {
    const auto handle = std::bit_cast<HANDLE>(thread.handle);
    const DWORD result = WaitForSingleObject(handle, INFINITE);
    ORION_VERIFY(result == WAIT_OBJECT_0, "WaitForSingleObject trả {}", result);
    static_cast<void>(CloseHandle(handle));
}

void set_current_thread_name(const char* name) noexcept {
    // Tên chỉ gồm ASCII in được (Thread::start đã kiểm), nên đổi sang UTF-16 bằng chép từng byte.
    std::array<wchar_t, kMaxThreadName + 1> wide{};
    for (usize i = 0; i < kMaxThreadName && name[i] != '\0'; ++i) {
        wide[i] = static_cast<wchar_t>(name[i]);
    }
    static_cast<void>(SetThreadDescription(GetCurrentThread(), wide.data()));
}

usize current_thread_name(std::array<char, kMaxThreadName + 1>& buffer) noexcept {
    buffer.fill('\0');
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description))) {
        return 0;
    }
    usize length = 0;
    while (length < kMaxThreadName && description[length] != L'\0') {
        buffer[length] = static_cast<char>(description[length]);
        ++length;
    }
    static_cast<void>(LocalFree(description));
    return length;
}

}  // namespace orion::jobs::detail
