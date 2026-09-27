#include "engine/io/streamer.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/queue.hpp"
#include "engine/core/types.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/vfs.hpp"
#include "engine/jobs/thread.hpp"

#include <algorithm>
#include <atomic>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace orion::io {

struct Streamer::State {
    State(const Vfs& source, const usize limit)
        : vfs(&source), capacity(limit), requests(limit), results(limit) {}

    const Vfs* vfs;
    usize capacity;
    // Luồng gọi đẩy, luồng IO lấy.
    core::SpscRing<std::optional<StreamRequest>> requests;
    // Luồng IO đẩy, luồng gọi lấy. Không bao giờ đầy: submit giữ số yêu cầu chưa poll dưới
    // capacity, và sức chứa của hàng đợi không nhỏ hơn capacity.
    core::SpscRing<StreamResult> results;
    // Tăng sau mỗi yêu cầu mới và khi dừng; luồng IO ngủ trên nó khi hàng yêu cầu rỗng.
    // Đồng bộ: bên tăng dùng release sau khi đẩy yêu cầu (hay đặt cờ dừng); luồng IO đọc bằng
    // acquire rồi mới thử lấy, nên không bỏ lỡ yêu cầu nào: nếu nó đọc giá trị cũ thì wait trả về
    // ngay khi giá trị đã đổi, hoặc được notify đánh thức sau lần tăng.
    std::atomic<u32> wake{0};
};

Streamer::Streamer(std::unique_ptr<State> state, jobs::Thread thread) noexcept
    : state_(std::move(state)), thread_(std::move(thread)) {}

Streamer::Streamer(Streamer&& other) noexcept
    : state_(std::move(other.state_)),
      thread_(std::move(other.thread_)),
      pending_(std::exchange(other.pending_, 0)) {}

Streamer& Streamer::operator=(Streamer&& other) noexcept {
    if (this != &other) {
        stop();
        state_ = std::move(other.state_);
        thread_ = std::move(other.thread_);
        pending_ = std::exchange(other.pending_, 0);
    }
    return *this;
}

Streamer::~Streamer() {
    stop();
}

Result<Streamer> Streamer::start(const Vfs& vfs, const usize capacity) {
    auto state = std::make_unique<State>(vfs, std::max<usize>(capacity, 1));
    State* const shared = state.get();
    Result<jobs::Thread> thread = jobs::Thread::start(
        "orion-io", [shared](const jobs::StopToken stop) { run(*shared, stop); });
    if (!thread) {
        return std::unexpected(thread.error());
    }
    return Streamer(std::move(state), std::move(*thread));
}

void Streamer::run(State& state, const jobs::StopToken stop) noexcept {
    PakReadContext context;
    u32 seen = state.wake.load(std::memory_order_acquire);
    while (!stop.stop_requested()) {
        std::optional<StreamRequest> request;
        if (!state.requests.try_pop(request)) {
            state.wake.wait(seen, std::memory_order_acquire);
            seen = state.wake.load(std::memory_order_acquire);
            continue;
        }
        ORION_VERIFY(request.has_value(), "streamer: ô yêu cầu rỗng");
        StreamResult result{request->ticket, state.vfs->read_all(request->path, context)};
        const bool pushed = state.results.try_push(std::move(result));
        ORION_VERIFY(pushed, "streamer: hàng kết quả đầy");
    }
}

void Streamer::stop() noexcept {
    if (state_ == nullptr) {
        return;
    }
    thread_.request_stop();
    state_->wake.fetch_add(1, std::memory_order_release);
    state_->wake.notify_all();
    thread_.join();
    state_.reset();
    pending_ = 0;
}

bool Streamer::submit(const StreamRequest& request) noexcept {
    if (state_ == nullptr || pending_ >= state_->capacity) {
        return false;
    }
    const bool pushed = state_->requests.try_push(std::optional<StreamRequest>(request));
    ORION_VERIFY(pushed, "streamer: hàng yêu cầu đầy khi chưa đủ capacity");
    ++pending_;
    state_->wake.fetch_add(1, std::memory_order_release);
    state_->wake.notify_one();
    return true;
}

usize Streamer::poll(const std::span<StreamResult> out) noexcept {
    if (state_ == nullptr) {
        return 0;
    }
    usize count = 0;
    while (count < out.size() && state_->results.try_pop(out[count])) {
        ++count;
    }
    pending_ -= count;
    return count;
}

}  // namespace orion::io
