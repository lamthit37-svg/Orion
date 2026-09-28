#include "game/server/lib/http/server.hpp"

#include "engine/core/error.hpp"
#include "engine/core/narrow.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"
#include "game/server/lib/http/detail/connection.hpp"
#include "game/server/lib/http/detail/core.hpp"
#include "game/server/lib/http/detail/listener.hpp"
#include "game/server/lib/http/detail/message.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace orion::http {
namespace {

using boost::system::error_code;
namespace ip = boost::asio::ip;

[[nodiscard]] bool valid_config(const ServerConfig& config) noexcept {
    return config.io_threads > 0 && config.workers > 0 && config.queue_capacity > 0 &&
           config.max_connections > 0 && config.check_interval >= core::Duration{} &&
           detail::valid_limits(config.limits);
}

}  // namespace

// Đồng bộ: io_, core_ tự đồng bộ; acceptor_, timer_, accepting_, next_id_ chỉ được chạm trên
// accept_strand_ sau khi run() trả về; stopped_ atomic; io_threads_, work_ chỉ start và stop chạm,
// trên luồng sở hữu Server.
class Server::Impl {
public:
    Impl(const ServerConfig& config, Handler handler, const core::MonotonicClock& clock,
         const core::WallClock& wall_clock);
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl();

    [[nodiscard]] Result<void> open(const ServerConfig& config);
    [[nodiscard]] Result<void> run(const ServerConfig& config);
    void stop() noexcept;
    void check_deadlines() const noexcept;
    [[nodiscard]] ServerStats stats() const noexcept;
    [[nodiscard]] u16 port() const noexcept { return port_; }

private:
    void accept();
    void on_accept(const error_code& error, ip::tcp::socket socket);
    void resume_accept();
    void arm_timer();

    // io_ khai đầu tiên nên bị huỷ sau cùng: mọi socket, timer và việc còn treo thuộc về nó.
    boost::asio::io_context io_;
    detail::Core core_;
    boost::asio::strand<boost::asio::io_context::executor_type> accept_strand_;
    ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer timer_;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
    std::vector<jobs::Thread> io_threads_;
    core::Duration check_interval_;
    std::atomic<bool> stopped_{false};
    bool accepting_ = false;
    u64 next_id_ = 0;
    u16 port_ = 0;
};

Server::Impl::Impl(const ServerConfig& config, Handler handler, const core::MonotonicClock& clock,
                   const core::WallClock& wall_clock)
    : io_(narrow<int>(config.io_threads)),
      core_(config, std::move(handler), clock, wall_clock),
      accept_strand_(boost::asio::make_strand(io_)),
      acceptor_(accept_strand_),
      timer_(accept_strand_),
      check_interval_(config.check_interval) {
    core_.connection_closed = [this] {
        boost::asio::post(accept_strand_, [this] { resume_accept(); });
    };
}

Server::Impl::~Impl() {
    stop();
}

Result<void> Server::Impl::open(const ServerConfig& config) {
    error_code error;
    const boost::asio::ip::address address = boost::asio::ip::make_address(config.address, error);
    if (error) {
        return fail(ErrorCode::InvalidArgument, "http: địa chỉ không phải IP dạng số");
    }
    const ip::tcp::endpoint endpoint(address, config.port);
    static_cast<void>(acceptor_.open(endpoint.protocol(), error));
    if (!error) {
        detail::configure_listener(acceptor_, error);
    }
    if (!error) {
        static_cast<void>(acceptor_.bind(endpoint, error));
    }
    if (!error) {
        static_cast<void>(
            acceptor_.listen(boost::asio::socket_base::max_listen_connections, error));
    }
    if (!error) {
        port_ = acceptor_.local_endpoint(error).port();
    }
    if (error) {
        return fail(ErrorCode::Io, "http: không mở được cổng nghe", error.value());
    }
    return {};
}

Result<void> Server::Impl::run(const ServerConfig& config) {
    if (Result<void> started = core_.workers.start(config.workers, config.queue_capacity);
        !started) {
        return started;
    }
    work_.emplace(io_.get_executor());
    io_threads_.reserve(config.io_threads);
    for (u32 i = 0; i < config.io_threads; ++i) {
        Result<jobs::Thread> thread = jobs::Thread::start(
            "http-io", [this](jobs::StopToken) { static_cast<void>(io_.run()); });
        if (!thread) {
            return std::unexpected(thread.error());
        }
        io_threads_.push_back(std::move(*thread));
    }
    boost::asio::post(accept_strand_, [this] {
        accept();
        arm_timer();
    });
    return {};
}

void Server::Impl::stop() noexcept {
    if (stopped_.exchange(true)) {
        return;
    }
    // Không nhận kết nối mới, không quét hạn nữa.
    boost::asio::post(accept_strand_, [this] {
        error_code ignored;
        static_cast<void>(acceptor_.close(ignored));
        timer_.cancel();
    });
    // Đóng mọi kết nối; thao tác đang chờ của chúng xong với operation_aborted.
    for (const std::shared_ptr<detail::Connection>& connection : core_.registry.close_all()) {
        connection->shutdown();
    }
    // Chờ handler đang chạy trả về; response của chúng tới một kết nối đã đóng thì bị bỏ.
    core_.workers.stop();
    // Hết việc thì io_context::run trả về: không kết nối, acceptor, timer nào còn thao tác chờ.
    work_.reset();
    for (jobs::Thread& thread : io_threads_) {
        thread.join();
    }
    io_threads_.clear();
}

void Server::Impl::check_deadlines() const noexcept {
    const core::MonoTime now = core_.clock->now();
    for (const std::shared_ptr<detail::Connection>& connection : core_.registry.snapshot()) {
        if (connection->deadline() <= now) {
            connection->expire(now);
        }
    }
}

ServerStats Server::Impl::stats() const noexcept {
    const detail::Counters& counters = core_.counters;
    // acquire: ghép với release của Connection::set_phase (core.hpp, Counters).
    constexpr auto kPhase = std::memory_order_acquire;
    // relaxed: đếm dồn chỉ để đọc.
    constexpr auto kCount = std::memory_order_relaxed;
    ServerStats out;
    out.waiting = counters.phases[0].load(kPhase);
    out.reading = counters.phases[1].load(kPhase);
    out.handling = counters.phases[2].load(kPhase);
    out.writing = counters.phases[3].load(kPhase);
    out.closing = counters.phases[4].load(kPhase);
    out.accepted = counters.accepted.load(kCount);
    out.requests = counters.requests.load(kCount);
    out.rejected = counters.rejected.load(kCount);
    out.timed_out = counters.timed_out.load(kCount);
    return out;
}

void Server::Impl::accept() {
    accepting_ = true;
    // Mỗi kết nối một strand riêng: handler của nó không bao giờ chạy song song với nhau.
    acceptor_.async_accept(boost::asio::make_strand(io_),
                           [this](const error_code& error, ip::tcp::socket socket) {
                               on_accept(error, std::move(socket));
                           });
}

void Server::Impl::on_accept(const error_code& error, ip::tcp::socket socket) {
    accepting_ = false;
    if (stopped_.load() || error == boost::asio::error::operation_aborted) {
        return;
    }
    if (error) {
        // Hết file descriptor hay bộ nhớ: nhận tiếp ngay thì chỉ lặp lỗi; lần quét hạn sau thử lại.
        core_.errors.accept_failed(error.message());
        return;
    }
    error_code ignored;
    static_cast<void>(socket.set_option(ip::tcp::no_delay(true), ignored));
    const u64 id = next_id_++;
    auto connection = std::make_shared<detail::Connection>(core_, std::move(socket), id);
    if (!core_.registry.add(id, connection)) {
        connection->shutdown();
        return;
    }
    core_.counters.accepted.fetch_add(1, std::memory_order_relaxed);
    connection->start();
    resume_accept();
}

// Nhận tiếp nếu chưa có lượt nhận đang chờ và còn chỗ; đủ max_connections thì kết nối mới chờ trong
// hàng listen của hệ điều hành tới khi có kết nối đóng.
void Server::Impl::resume_accept() {
    if (!accepting_ && !stopped_.load() && acceptor_.is_open() &&
        core_.registry.size() < core_.max_connections) {
        accept();
    }
}

void Server::Impl::arm_timer() {
    if (stopped_.load() || check_interval_ <= core::Duration{}) {
        return;
    }
    timer_.expires_after(std::chrono::nanoseconds(check_interval_.as_nanoseconds()));
    timer_.async_wait([this](const error_code& error) {
        if (error || stopped_.load()) {
            return;
        }
        check_deadlines();
        resume_accept();
        arm_timer();
    });
}

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Server::~Server() {
    stop();
}

Result<std::unique_ptr<Server>> Server::start(const ServerConfig& config, Handler handler,
                                              const core::MonotonicClock& clock,
                                              const core::WallClock& wall_clock) {
    if (!valid_config(config) || !handler) {
        return fail(ErrorCode::InvalidArgument, "http: cấu hình server sai");
    }
    auto impl = std::make_unique<Impl>(config, std::move(handler), clock, wall_clock);
    if (Result<void> opened = impl->open(config); !opened) {
        return std::unexpected(opened.error());
    }
    if (Result<void> ran = impl->run(config); !ran) {
        impl->stop();
        return std::unexpected(ran.error());
    }
    return std::make_unique<Server>(std::move(impl));
}

u16 Server::port() const noexcept {
    return impl_->port();
}

ServerStats Server::stats() const noexcept {
    return impl_->stats();
}

void Server::check_deadlines() noexcept {
    impl_->check_deadlines();
}

void Server::stop() noexcept {
    impl_->stop();
}

}  // namespace orion::http
