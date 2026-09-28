#include "game/server/lib/http/detail/connection.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/detail/core.hpp"
#include "game/server/lib/http/detail/message.hpp"
#include "game/server/lib/http/detail/response.hpp"
#include "game/server/lib/http/detail/worker_pool.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace orion::http::detail {
namespace {

using boost::system::error_code;

constexpr i64 kNoDeadline = std::numeric_limits<i64>::max();
// Byte tối đa bỏ đi khi đóng êm; client gửi hơn thì đóng luôn.
constexpr usize kMaxDrainBytes = usize{1} << 20U;
// Retry-After của 503 khi hàng đợi handler đầy.
constexpr core::Duration kOverloadRetry = core::Duration::seconds(1);

// Mã lỗi trong body của response lỗi mà server tự trả (docs/formats/http.md, mục "Lỗi").
[[nodiscard]] std::string_view error_code_for(const Status status) noexcept {
    switch (status) {
        case Status::BadRequest:
            return "bad_request";
        case Status::RequestTimeout:
            return "request_timeout";
        case Status::LengthRequired:
            return "length_required";
        case Status::ContentTooLarge:
            return "content_too_large";
        case Status::ExpectationFailed:
            return "expectation_failed";
        case Status::RequestHeaderFieldsTooLarge:
            return "header_too_large";
        case Status::NotImplemented:
            return "not_implemented";
        case Status::HttpVersionNotSupported:
            return "version_not_supported";
        case Status::ServiceUnavailable:
            return "overloaded";
        default:
            return "internal";
    }
}

// Ô của ServerStats cho một pha; nullopt với New, Closed.
[[nodiscard]] std::optional<usize> phase_slot(const Phase phase) noexcept {
    switch (phase) {
        case Phase::Waiting:
            return 0;
        case Phase::Reading:
            return 1;
        case Phase::Handling:
            return 2;
        case Phase::Writing:
            return 3;
        case Phase::Draining:
            return 4;
        case Phase::New:
        case Phase::Closed:
            return std::nullopt;
    }
    return std::nullopt;
}

// Việc của worker: giữ kết nối và request tới khi handler trả về.
class HandlerTask final : public Task {
public:
    HandlerTask(std::shared_ptr<Connection> connection, std::unique_ptr<Message> message,
                const core::MonoTime deadline) noexcept
        : connection_(std::move(connection)), message_(std::move(message)), deadline_(deadline) {}

    void run() noexcept override { connection_->handle(*message_, deadline_); }

private:
    std::shared_ptr<Connection> connection_;
    std::unique_ptr<Message> message_;
    core::MonoTime deadline_;
};

}  // namespace

Connection::Connection(Core& core, boost::asio::ip::tcp::socket socket, const u64 id)
    : core_(&core),
      executor_(socket.get_executor()),
      socket_(std::move(socket)),
      buffer_(core.limits.max_header_bytes + core.limits.max_body_bytes + kReadChunk),
      deadline_ns_(kNoDeadline),
      id_(id) {}

void Connection::start() {
    boost::asio::post(executor_, [self = shared_from_this()] { self->wait_for_request(); });
}

void Connection::expire(const core::MonoTime time) {
    boost::asio::post(executor_, [self = shared_from_this(), time] { self->on_expire(time); });
}

void Connection::shutdown() {
    boost::asio::post(executor_, [self = shared_from_this()] { self->close(); });
}

core::MonoTime Connection::deadline() const noexcept {
    return core::MonoTime::from_nanoseconds(deadline_ns_.load(std::memory_order_relaxed));
}

void Connection::handle(const Message& message, const core::MonoTime deadline) noexcept {
    Response response = core_->handler(Request(message, deadline));
    boost::asio::post(executor_,
                      [self = shared_from_this(), response = std::move(response)]() mutable {
                          self->respond(std::move(response));
                      });
}

// NOLINTBEGIN(misc-no-recursion): đồ thị gọi tĩnh thấy vòng read_header → thao tác của Beast →
// hàm nhận kết quả → on_header → … → read_header, vì thao tác tổng hợp của Beast có nhánh gọi thẳng
// hàm nhận kết quả. Lúc chạy không có đệ quy: Asio và Beast không bao giờ gọi hàm nhận kết quả từ
// trong hàm bắt đầu thao tác (xong ngay thì post), nên mỗi bước chạy trên một khung stack mới của
// vòng sự kiện và stack không lớn dần (lý do X.3 cấm đệ quy).
void Connection::wait_for_request() {
    if (phase_ == Phase::Closed) {
        return;
    }
    set_deadline(now() + core_->limits.idle_timeout);
    set_phase(Phase::Waiting);
    // Request kế tiếp đã tới cùng lượt đọc của request trước (pipelining).
    if (buffer_.size() > 0) {
        read_header();
        return;
    }
    socket_.async_read_some(
        buffer_.prepare(kReadChunk),
        [self = shared_from_this()](const error_code& error, const usize bytes) {
            self->on_first_bytes(error, bytes);
        });
}

void Connection::on_first_bytes(const error_code& error, const usize bytes) {
    if (phase_ == Phase::Closed) {
        return;
    }
    if (error || timed_out_) {
        close();
        return;
    }
    buffer_.commit(bytes);
    read_header();
}

void Connection::read_header() {
    set_deadline(now() + core_->limits.read_timeout);
    set_phase(Phase::Reading);
    parser_.emplace();
    configure(*parser_, core_->limits);
    boost::beast::http::async_read_header(
        socket_, buffer_, *parser_,
        [self = shared_from_this()](const error_code& error, const usize bytes) {
            self->on_header(error, bytes);
        });
}

void Connection::on_header(const error_code& error, const usize bytes) {
    if (phase_ == Phase::Closed) {
        return;
    }
    if (timed_out_) {
        reject(Status::RequestTimeout);
        return;
    }
    if (error) {
        fail_read(error);
        return;
    }
    if (const std::optional<Status> status = check_header(*parser_, bytes, core_->limits)) {
        reject(*status);
        return;
    }
    if (parser_->is_done()) {
        dispatch();
        return;
    }
    boost::beast::http::async_read(
        socket_, buffer_, *parser_,
        [self = shared_from_this()](const error_code& body_error, const usize /*bytes*/) {
            self->on_body(body_error);
        });
}

void Connection::on_body(const error_code& error) {
    if (phase_ == Phase::Closed) {
        return;
    }
    if (timed_out_) {
        reject(Status::RequestTimeout);
        return;
    }
    if (error) {
        fail_read(error);
        return;
    }
    dispatch();
}

void Connection::fail_read(const error_code& error) {
    if (const std::optional<Status> status = read_error_status(error)) {
        reject(*status);
    } else {
        close();
    }
}

void Connection::dispatch() {
    ORION_ASSERT(parser_.has_value(), "http: dispatch khi chưa đọc request");
    if (!parser_) {
        close();
        return;
    }
    std::unique_ptr<Message> message = finish(*parser_);
    parser_.reset();
    head_ = message->method == Method::Head;
    keep_alive_ = message->request.keep_alive();
    set_deadline(core::MonoTime::from_nanoseconds(kNoDeadline));
    set_phase(Phase::Handling);
    const core::MonoTime deadline = now() + core_->limits.handler_timeout;
    if (core_->workers.try_submit(
            std::make_unique<HandlerTask>(shared_from_this(), std::move(message), deadline))) {
        core_->counters.requests.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    core_->counters.rejected.fetch_add(1, std::memory_order_relaxed);
    Response busy =
        error_response(Status::ServiceUnavailable, error_code_for(Status::ServiceUnavailable));
    busy.retry_after = kOverloadRetry;
    respond(std::move(busy));
}

void Connection::respond(Response response) {
    if (phase_ == Phase::Closed) {
        return;
    }
    ++served_;
    const bool keep_alive =
        keep_alive_ && !response.close && served_ < core_->limits.max_requests_per_connection;
    const Framing framing{.head = head_, .keep_alive = keep_alive, .now = core_->wall_clock->now()};
    const Status status = response.status;
    if (!build(std::move(response), framing, response_)) {
        core_->errors.invalid_response(status);
        const Result<void> fallback =
            build(error_response(Status::InternalServerError, "internal"), framing, response_);
        ORION_VERIFY(fallback.has_value(), "http: không dựng được response 500 của chính server");
    }
    set_deadline(now() + core_->limits.write_timeout);
    set_phase(Phase::Writing);
    boost::beast::http::async_write(
        socket_, response_,
        [self = shared_from_this(), keep_alive](const error_code& error, const usize /*bytes*/) {
            self->on_write(error, keep_alive);
        });
}

void Connection::on_write(const error_code& error, const bool keep_alive) {
    if (phase_ == Phase::Closed) {
        return;
    }
    if (error || timed_out_) {
        close();
        return;
    }
    // Trả bộ nhớ của body ngay, không giữ tới response kế tiếp.
    response_ = BeastResponse{};
    if (keep_alive) {
        wait_for_request();
    } else {
        linger();
    }
}

// Server tự trả lỗi rồi đóng kết nối (X.14): byte còn lại của request không đọc nữa.
void Connection::reject(const Status status) {
    if (status == Status::RequestTimeout) {
        core_->counters.timed_out.fetch_add(1, std::memory_order_relaxed);
    }
    core_->counters.rejected.fetch_add(1, std::memory_order_relaxed);
    // Response cho HEAD không có body, kể cả response lỗi (RFC 9110 §9.3.2).
    head_ = parser_ && parser_->is_header_done() &&
            parser_->get().method() == boost::beast::http::verb::head;
    keep_alive_ = false;
    timed_out_ = false;
    parser_.reset();
    respond(error_response(status, error_code_for(status)));
}
// NOLINTEND(misc-no-recursion)

// Đóng êm: đóng chiều gửi rồi đọc bỏ những gì client còn gửi tới khi client đóng, để response vừa
// ghi không bị TCP reset xoá khỏi bộ đệm nhận của client.
void Connection::linger() {
    set_deadline(now() + core_->limits.linger_timeout);
    set_phase(Phase::Draining);
    error_code ignored;
    static_cast<void>(socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored));
    drained_ = 0;
    drain();
}

void Connection::drain() {
    socket_.async_read_some(
        boost::asio::buffer(drain_buffer_),
        [self = shared_from_this()](const error_code& error, const usize bytes) {
            self->on_drain(error, bytes);
        });
}

void Connection::on_drain(const error_code& error, const usize bytes) {
    if (phase_ == Phase::Closed) {
        return;
    }
    drained_ += bytes;
    if (error || timed_out_ || drained_ > kMaxDrainBytes) {
        close();
        return;
    }
    drain();
}

void Connection::on_expire(const core::MonoTime time) {
    // Kiểm lại trên strand: hạn có thể đã đổi sau khi việc quét hạn đọc nó.
    if (phase_ == Phase::New || phase_ == Phase::Closed || phase_ == Phase::Handling ||
        timed_out_ || deadline() > time) {
        return;
    }
    timed_out_ = true;
    if (phase_ != Phase::Reading) {
        // Chờ request, ghi hay đóng êm: đóng luôn, mọi lượt đọc, ghi sau đó lỗi ngay.
        close();
        return;
    }
    // Đang đọc: phải trả 408 trước khi đóng. cancel chỉ huỷ lượt đọc đang chờ trong reactor; lượt
    // mà Beast bắt đầu ngay sau một lượt vừa xong thì không (đo được với lượt ghi: test
    // ClientThatStopsReadingIsClosedAfterTheWriteTimeout treo khi chỉ cancel). Đóng chiều nhận làm
    // mọi lượt đọc, đang chờ hay sắp tới, xong ngay với EOF, và hàm nhận kết quả thấy timed_out_.
    error_code ignored;
    static_cast<void>(socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_receive, ignored));
    static_cast<void>(socket_.cancel(ignored));
}

void Connection::close() {
    if (phase_ == Phase::Closed) {
        return;
    }
    if (timed_out_ && phase_ != Phase::Draining) {
        core_->counters.timed_out.fetch_add(1, std::memory_order_relaxed);
    }
    set_deadline(core::MonoTime::from_nanoseconds(kNoDeadline));
    set_phase(Phase::Closed);
    error_code ignored;
    static_cast<void>(socket_.close(ignored));
    parser_.reset();
    core_->registry.remove(id_);
    core_->connection_closed();
}

void Connection::set_phase(const Phase next) noexcept {
    if (const std::optional<usize> slot = phase_slot(phase_)) {
        // relaxed: giảm không công bố gì (core.hpp, Counters).
        core_->counters.phases.at(*slot).fetch_sub(1, std::memory_order_relaxed);
    }
    phase_ = next;
    if (const std::optional<usize> slot = phase_slot(next)) {
        // release: công bố hạn vừa đặt cho pha này cùng với pha (core.hpp, Counters).
        core_->counters.phases.at(*slot).fetch_add(1, std::memory_order_release);
    }
}

void Connection::set_deadline(const core::MonoTime deadline) noexcept {
    deadline_ns_.store(deadline.nanoseconds(), std::memory_order_relaxed);
}

core::MonoTime Connection::now() const noexcept {
    return core_->clock->now();
}

}  // namespace orion::http::detail
