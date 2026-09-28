#pragma once

// Phần của server_http mà mọi kết nối dùng chung (nội bộ): cấu hình, đồng hồ, handler, worker, danh
// sách kết nối, số đo.

#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/detail/worker_pool.hpp"
#include "game/server/lib/http/server.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orion::http::detail {

class Connection;

// Pha của một kết nối. New (chưa start) và Closed không được đếm trong số đo.
enum class Phase : u8 {
    New,
    Waiting,
    Reading,
    Handling,
    Writing,
    Draining,
    Closed,
};

// Số đo của server (ServerStats). Đếm dồn dùng relaxed: chỉ để đọc, không nối với dữ liệu nào khác.
// Số kết nối theo pha dùng release khi tăng và acquire khi đọc (Server::stats): kết nối đặt hạn của
// pha mới trước khi tăng, nên ai thấy nó ở pha đó thì thấy luôn hạn đó.
struct Counters {
    // Theo ô của ServerStats: waiting, reading, handling, writing, closing.
    std::array<std::atomic<usize>, 5> phases{};
    std::atomic<u64> accepted{0};
    std::atomic<u64> requests{0};
    std::atomic<u64> rejected{0};
    std::atomic<u64> timed_out{0};
};

// Kết nối đang mở, để quét hạn và để đóng khi dừng. Giữ weak_ptr: kết nối sống nhờ các thao tác
// Asio đang chờ của nó (connection.hpp), danh sách không kéo dài đời nó. Đồng bộ: mọi trường dưới
// mutex_.
class Registry {
public:
    // false khi server đang dừng: bên gọi đóng kết nối ngay.
    [[nodiscard]] bool add(u64 id, const std::shared_ptr<Connection>& connection);
    void remove(u64 id);
    [[nodiscard]] usize size() const;
    [[nodiscard]] std::vector<std::shared_ptr<Connection>> snapshot() const;
    // Từ giờ add luôn trả false; trả mọi kết nối còn mở để bên gọi đóng.
    [[nodiscard]] std::vector<std::shared_ptr<Connection>> close_all();

private:
    // Tiền điều kiện: đang giữ mutex_.
    [[nodiscard]] std::vector<std::shared_ptr<Connection>> live_locked() const;

    mutable std::mutex mutex_;
    std::unordered_map<u64, std::weak_ptr<Connection>> connections_;
    bool stopping_ = false;
};

// Log lỗi của handler, có giới hạn tần suất (X.5), gọi được từ mọi strand. Đồng bộ: limiter_ dưới
// mutex_.
class ErrorLog {
public:
    explicit ErrorLog(const core::MonotonicClock& clock) noexcept;

    // Handler trả response mà server không gửi được (header sai): server gửi 500 thay nó.
    void invalid_response(Status status) noexcept;
    // Acceptor lỗi (hết file descriptor...): server thử nhận lại ở lần quét hạn kế tiếp.
    void accept_failed(std::string_view reason) noexcept;

private:
    std::mutex mutex_;
    core::LogRateLimiter limiter_;
};

// Sống lâu hơn mọi kết nối còn chạy: Server::stop join mọi luồng IO và worker trước khi huỷ nó.
// Mọi trường không đổi sau khi server chạy, trừ các đối tượng tự đồng bộ (workers, registry,
// counters, errors).
struct Core {
    Core(const ServerConfig& config, Handler handler_in, const core::MonotonicClock& clock_in,
         const core::WallClock& wall_clock_in) noexcept;

    Limits limits;
    u32 max_connections;
    const core::MonotonicClock* clock;  // không null
    const core::WallClock* wall_clock;  // không null
    Handler handler;
    WorkerPool workers;
    Registry registry;
    Counters counters;
    ErrorLog errors;
    // Gọi trên strand của một kết nối vừa đóng, để acceptor nhận tiếp nếu đã ngừng vì đủ kết nối.
    // Đặt một lần trước khi server nhận kết nối đầu tiên.
    std::function<void()> connection_closed;
};

}  // namespace orion::http::detail
