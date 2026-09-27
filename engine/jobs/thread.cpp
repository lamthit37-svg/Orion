#include "engine/jobs/thread.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <expected>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace orion::jobs {
namespace {

[[nodiscard]] bool valid_thread_name(const std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxThreadName) {
        return false;
    }
    return std::ranges::all_of(name, [](const char c) { return c >= ' ' && c <= '~'; });
}

}  // namespace

void detail::ThreadState::enter() noexcept {
    set_current_thread_name(name.data());
    run(StopToken(stop));
}

Thread::Thread(Thread&& other) noexcept
    : state_(std::move(other.state_)), native_(std::exchange(other.native_, {})) {}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        request_stop();
        join();
        state_ = std::move(other.state_);
        native_ = std::exchange(other.native_, {});
    }
    return *this;
}

Thread::~Thread() {
    request_stop();
    join();
}

Result<Thread> Thread::start_state(const std::string_view name,
                                   std::unique_ptr<detail::ThreadState> state) {
    ORION_ASSERT(valid_thread_name(name), "tên luồng phải có 1..{} byte ASCII in được: '{}'",
                 kMaxThreadName, name);
    std::ranges::copy(name, state->name.begin());
    const Result<detail::NativeThread> native = detail::start_native_thread(*state);
    if (!native) {
        return std::unexpected(native.error());
    }
    Thread thread;
    thread.state_ = std::move(state);
    thread.native_ = *native;
    return thread;
}

void Thread::request_stop() noexcept {
    if (state_ != nullptr) {
        // release: ghép với acquire trong StopToken::stop_requested.
        state_->stop.store(true, std::memory_order_release);
    }
}

void Thread::join() noexcept {
    if (state_ != nullptr) {
        detail::join_native_thread(native_);
        native_ = {};
        state_.reset();
    }
}

std::string_view Thread::name() const noexcept {
    return state_ == nullptr ? std::string_view{} : std::string_view(state_->name.data());
}

u32 hardware_thread_count() noexcept {
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0 ? 1U : static_cast<u32>(count);
}

std::string_view current_thread_name(std::array<char, kMaxThreadName + 1>& buffer) noexcept {
    const usize length = detail::current_thread_name(buffer);
    return {buffer.data(), length};
}

}  // namespace orion::jobs
