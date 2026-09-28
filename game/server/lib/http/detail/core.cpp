#include "game/server/lib/http/detail/core.hpp"

#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/server.hpp"

#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::http::detail {

bool Registry::add(const u64 id, const std::shared_ptr<Connection>& connection) {
    const std::scoped_lock lock(mutex_);
    if (stopping_) {
        return false;
    }
    connections_.emplace(id, connection);
    return true;
}

void Registry::remove(const u64 id) {
    const std::scoped_lock lock(mutex_);
    connections_.erase(id);
}

usize Registry::size() const {
    const std::scoped_lock lock(mutex_);
    return connections_.size();
}

std::vector<std::shared_ptr<Connection>> Registry::snapshot() const {
    const std::scoped_lock lock(mutex_);
    return live_locked();
}

std::vector<std::shared_ptr<Connection>> Registry::close_all() {
    // Một lần khoá: kết nối nào add thành công đều nằm trong kết quả.
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
    std::vector<std::shared_ptr<Connection>> out = live_locked();
    connections_.clear();
    return out;
}

std::vector<std::shared_ptr<Connection>> Registry::live_locked() const {
    std::vector<std::shared_ptr<Connection>> out;
    out.reserve(connections_.size());
    for (const auto& [id, weak] : connections_) {
        if (std::shared_ptr<Connection> connection = weak.lock()) {
            out.push_back(std::move(connection));
        }
    }
    return out;
}

ErrorLog::ErrorLog(const core::MonotonicClock& clock) noexcept
    : limiter_(clock, core::Duration::seconds(10), 5) {}

void ErrorLog::invalid_response(const Status status) noexcept {
    const std::scoped_lock lock(mutex_);
    if (limiter_.allow()) {
        core::log_error(
            "http: handler trả response {} có header sai hay header server tự quản; đã gửi 500 "
            "thay (bỏ {} lần trước đó)",
            std::to_underlying(status), limiter_.take_suppressed());
    }
}

void ErrorLog::accept_failed(const std::string_view reason) noexcept {
    const std::scoped_lock lock(mutex_);
    if (limiter_.allow()) {
        core::log_warn("http: nhận kết nối lỗi: {}; thử lại ở lần quét hạn kế tiếp (bỏ {} lần)",
                       reason, limiter_.take_suppressed());
    }
}

Core::Core(const ServerConfig& config, Handler handler_in, const core::MonotonicClock& clock_in,
           const core::WallClock& wall_clock_in) noexcept
    : limits(config.limits),
      max_connections(config.max_connections),
      clock(&clock_in),
      wall_clock(&wall_clock_in),
      handler(std::move(handler_in)),
      errors(clock_in) {}

}  // namespace orion::http::detail
