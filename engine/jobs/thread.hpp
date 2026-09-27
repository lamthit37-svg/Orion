#pragma once

// Luồng hệ điều hành có tên (ADR 0007): chỉ engine/jobs tạo luồng, và mọi luồng có tên để thấy
// được trong profiler, debugger và log. Module khác xin luồng qua Thread::start.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace orion::jobs {

// Tên dài nhất, tính bằng byte: giới hạn của Linux (16 byte gồm ký tự kết thúc).
inline constexpr usize kMaxThreadName = 15;

namespace detail {
class ThreadState;
}  // namespace detail

// Cờ dừng của một Thread, đưa cho thân luồng. Đồng bộ: Thread ghi cờ bằng release, thân luồng đọc
// bằng acquire, nên mọi thứ bên yêu cầu dừng ghi trước đó đều thấy được sau khi thấy cờ.
class StopToken {
public:
    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_->load(std::memory_order_acquire);
    }

private:
    friend class detail::ThreadState;
    explicit StopToken(const std::atomic<bool>& flag) noexcept : flag_(&flag) {}

    const std::atomic<bool>* flag_;
};

namespace detail {

// Trạng thái dùng chung giữa Thread và luồng nó tạo; sống tới khi Thread join xong.
class ThreadState {
public:
    ThreadState() = default;
    ThreadState(const ThreadState&) = delete;
    ThreadState& operator=(const ThreadState&) = delete;
    ThreadState(ThreadState&&) = delete;
    ThreadState& operator=(ThreadState&&) = delete;
    virtual ~ThreadState() = default;

    // Chạy trên luồng mới: đặt tên luồng rồi gọi thân luồng.
    void enter() noexcept;

    std::atomic<bool> stop{false};
    std::array<char, kMaxThreadName + 1> name{};

private:
    virtual void run(StopToken token) noexcept = 0;
};

template <class Body>
class ThreadStateFor final : public ThreadState {
public:
    explicit ThreadStateFor(Body body) : body_(std::move(body)) {}

private:
    void run(const StopToken token) noexcept override { body_(token); }

    Body body_;
};

// Luồng gốc của nền tảng, cài trong win/, linux/, apple/.
struct NativeThread {
    u64 handle = 0;
};

[[nodiscard]] Result<NativeThread> start_native_thread(ThreadState& state) noexcept;
void join_native_thread(NativeThread thread) noexcept;
void set_current_thread_name(const char* name) noexcept;
// Ghi tên của luồng đang chạy vào buffer (có ký tự kết thúc); trả số byte của tên.
[[nodiscard]] usize current_thread_name(std::array<char, kMaxThreadName + 1>& buffer) noexcept;

}  // namespace detail

// Một luồng hệ điều hành có tên, sở hữu duy nhất (X.7). Huỷ Thread thì yêu cầu dừng rồi chờ luồng
// kết thúc; thân luồng phải tự trả về khi thấy stop_requested().
class Thread {
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;
    ~Thread();

    // Tạo luồng tên `name` (1..15 byte ASCII in được, do lập trình viên đặt nên sai là lỗi lập
    // trình) chạy body(StopToken). Trả lỗi ResourceExhausted khi hệ điều hành từ chối tạo luồng.
    template <class Body>
        requires std::is_invocable_v<Body&, StopToken>
    [[nodiscard]] static Result<Thread> start(std::string_view name, Body body) {
        return start_state(name, std::make_unique<detail::ThreadStateFor<Body>>(std::move(body)));
    }

    // Đặt cờ dừng; không chờ.
    void request_stop() noexcept;
    // Chờ luồng kết thúc. Gọi được nhiều lần; lần sau không làm gì.
    void join() noexcept;
    [[nodiscard]] bool joinable() const noexcept { return state_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept;

private:
    [[nodiscard]] static Result<Thread> start_state(std::string_view name,
                                                    std::unique_ptr<detail::ThreadState> state);

    std::unique_ptr<detail::ThreadState> state_;
    detail::NativeThread native_{};
};

// Số luồng phần cứng; 1 nếu hệ điều hành không cho biết.
[[nodiscard]] u32 hardware_thread_count() noexcept;

// Tên của luồng đang chạy, như hệ điều hành thấy.
[[nodiscard]] std::string_view current_thread_name(
    std::array<char, kMaxThreadName + 1>& buffer) noexcept;

}  // namespace orion::jobs
