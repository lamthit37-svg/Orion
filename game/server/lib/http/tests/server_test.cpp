// Server qua loopback (X.4: không mạng ngoài loopback): client chặn bằng Asio. Hạn chỉ chạy theo
// đồng hồ giả: server không tự quét hạn (check_interval = 0), test tiến đồng hồ rồi gọi
// check_deadlines, sau khi thấy kết nối ở đúng pha qua stats() (thấy pha thì thấy hạn của pha,
// server.hpp).

#include "game/server/lib/http/server.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/json.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/system/error_code.hpp>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace orion::http {
namespace {

namespace asio = boost::asio;
namespace beast_http = boost::beast::http;
namespace ip = asio::ip;
using Reply = beast_http::response<beast_http::string_body>;
using boost::system::error_code;

constexpr std::string_view kDate = "Sun, 06 Nov 1994 08:49:37 GMT";

// Hỏi lại tới khi `condition` đúng; không sleep, không đồng hồ (X.4). Nếu nó không bao giờ đúng,
// ctest dừng test theo TIMEOUT.
template <class Condition>
void wait_until(Condition condition) {
    while (!condition()) {
        std::this_thread::yield();
    }
}

[[nodiscard]] std::string_view field(const Reply& reply, const beast_http::field name) {
    const auto value = reply[name];
    return {value.data(), value.size()};
}

// Client HTTP chặn, một kết nối loopback.
class Client {
public:
    // `receive_buffer` khác 0: đặt SO_RCVBUF trước khi kết nối, để bộ đệm nhận không tự nới (Linux
    // nới tới tcp_rmem, có máy 32 MiB) và server ghi kẹt được khi client không đọc.
    explicit Client(const u16 port, const i32 receive_buffer = 0) : socket_(io_) {
        error_code error;
        static_cast<void>(socket_.open(ip::tcp::v4(), error));
        if (!error && receive_buffer > 0) {
            static_cast<void>(
                socket_.set_option(asio::socket_base::receive_buffer_size(receive_buffer), error));
        }
        if (!error) {
            static_cast<void>(
                socket_.connect(ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), error));
        }
        connected_ = !error;
    }

    [[nodiscard]] bool connected() const { return connected_; }

    void send(const std::string_view bytes) {
        error_code error;
        asio::write(socket_, asio::buffer(bytes.data(), bytes.size()), error);
        EXPECT_FALSE(error) << error.message();
    }

    // Một response; mã trạng thái 0 khi kết nối đóng trước khi đọc đủ. `head`: response cho HEAD,
    // không có body dù có Content-Length.
    [[nodiscard]] Reply read(const bool head = false) {
        beast_http::response_parser<beast_http::string_body> parser;
        parser.body_limit(usize{64} * 1024 * 1024);
        parser.skip(head);
        error_code error;
        static_cast<void>(beast_http::read(socket_, buffer_, parser, error));
        if (error) {
            Reply closed;
            closed.result(0);
            return closed;
        }
        return parser.release();
    }

    // Mọi byte tới khi server đóng.
    [[nodiscard]] std::string read_to_end() {
        std::string out = boost::beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        std::array<char, 4'096> chunk{};
        error_code error;
        while (!error) {
            const usize n = socket_.read_some(asio::buffer(chunk), error);
            out.append(chunk.data(), n);
        }
        return out;
    }

    void close() {
        error_code ignored;
        static_cast<void>(socket_.close(ignored));
    }

private:
    asio::io_context io_;
    ip::tcp::socket socket_;
    boost::beast::flat_buffer buffer_;
    bool connected_ = false;
};

// Cửa cho handler chờ: test mở khi muốn handler trả về.
class Gate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        ++waiting_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
    }
    void wait_for_waiters(const u32 count) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this, count] { return waiting_ >= count; });
    }
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    u32 waiting_ = 0;
    bool open_ = false;
};

class ServerTest : public ::testing::Test {
protected:
    // Handler mặc định trả lại request dạng JSON: {"method","path","query","body","x"}. Mặc định
    // server không tự quét hạn; `periodic_sweep` giữ check_interval của config.
    void start(ServerConfig config = {}, const bool periodic_sweep = false) {
        if (!periodic_sweep) {
            config.check_interval = core::Duration{};
        }
        Result<std::unique_ptr<Server>> started = Server::start(
            config, [this](const Request& request) { return handle(request); }, clock_,
            wall_clock_);
        ASSERT_TRUE(started.has_value()) << std::format("{}", started.error());
        server_ = std::move(*started);
    }

    void set_handler(Handler handler) { handler_ = std::move(handler); }
    [[nodiscard]] Server& server() { return *server_; }
    [[nodiscard]] u16 port() const { return server_->port(); }
    [[nodiscard]] u64 handled() const { return handled_.load(); }
    [[nodiscard]] core::FakeMonotonicClock& clock() { return clock_; }
    [[nodiscard]] ServerStats stats() const { return server_->stats(); }

    // Tiến đồng hồ giả rồi quét hạn.
    void advance(const core::Duration step) {
        clock_.advance(step);
        server_->check_deadlines();
    }

    [[nodiscard]] static std::string echo(const Request& request) {
        json::Writer writer;
        writer.begin_object();
        writer.key("method");
        writer.integer(static_cast<i64>(std::to_underlying(request.method())));
        writer.key("path");
        writer.string(request.path());
        writer.key("query");
        writer.string(request.query());
        writer.key("body");
        writer.string(request.body());
        writer.key("x");
        writer.string(request.header("x-test").value_or("-"));
        writer.end_object();
        return writer.finish();
    }

private:
    Response handle(const Request& request) {
        handled_.fetch_add(1);
        if (handler_) {
            return handler_(request);
        }
        Response response;
        response.body = echo(request);
        return response;
    }

    core::FakeMonotonicClock clock_{core::MonoTime::from_nanoseconds(1'000'000'000)};
    core::FakeWallClock wall_clock_{core::WallTime::from_unix_seconds(784'111'777)};
    Handler handler_;
    std::atomic<u64> handled_{0};
    std::unique_ptr<Server> server_;
};

TEST_F(ServerTest, ServesRequestsOnAKeepAliveConnection) {
    start();
    Client client(port());
    ASSERT_TRUE(client.connected());
    client.send("GET /v1/a?x=1 HTTP/1.1\r\nHost: t\r\nX-Test: one\r\n\r\n");
    const Reply first = client.read();
    ASSERT_NE(first.result_int(), 0U);
    EXPECT_EQ(first.result_int(), 200U);
    EXPECT_TRUE(first.keep_alive());
    EXPECT_EQ(field(first, beast_http::field::date), kDate);
    EXPECT_EQ(field(first, beast_http::field::content_type), "application/json");
    EXPECT_EQ(field(first, beast_http::field::cache_control), "no-store");
    EXPECT_EQ(first.body(), R"({"method":0,"path":"/v1/a","query":"x=1","body":"","x":"one"})");

    client.send("POST /v1/b HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nabcd");
    const Reply second = client.read();
    ASSERT_NE(second.result_int(), 0U);
    EXPECT_EQ(second.body(), R"({"method":2,"path":"/v1/b","query":"","body":"abcd","x":"-"})");
    EXPECT_EQ(stats().accepted, 1U);
    EXPECT_EQ(stats().requests, 2U);
    EXPECT_EQ(handled(), 2U);
}

TEST_F(ServerTest, PipelinedRequestsAreAnsweredInOrder) {
    start();
    Client client(port());
    client.send(
        "GET /1 HTTP/1.1\r\nHost: t\r\n\r\nPUT /2 HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\n\r\n"
        "zGET /3 HTTP/1.1\r\nHost: t\r\n\r\n");
    for (const std::string_view expected :
         {R"({"method":0,"path":"/1","query":"","body":"","x":"-"})",
          R"({"method":3,"path":"/2","query":"","body":"z","x":"-"})",
          R"({"method":0,"path":"/3","query":"","body":"","x":"-"})"}) {
        const Reply reply = client.read();
        ASSERT_NE(reply.result_int(), 0U);
        EXPECT_EQ(reply.body(), expected);
    }
}

TEST_F(ServerTest, HeadSendsTheLengthWithoutTheBody) {
    set_handler([](const Request&) {
        Response response;
        response.content_type = "text/plain";
        response.body = "hello";
        return response;
    });
    start();
    Client client(port());
    client.send("HEAD / HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply head = client.read(true);
    ASSERT_NE(head.result_int(), 0U);
    EXPECT_EQ(field(head, beast_http::field::content_length), "5");
    // Khung vẫn đúng: request kế tiếp trên cùng kết nối đọc được body.
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply get = client.read();
    ASSERT_NE(get.result_int(), 0U);
    EXPECT_EQ(get.body(), "hello");
}

struct Rejection {
    std::string request;
    std::string_view status_line;
    std::string_view code;
};

// Server tự trả lỗi, không gọi handler, rồi đóng kết nối (X.14).
TEST_F(ServerTest, RejectsInvalidRequestsAndCloses) {
    ServerConfig config;
    config.limits.max_header_bytes = 512;
    config.limits.max_body_bytes = 16;
    start(config);
    const std::array<Rejection, 8> cases{{
        {"GET / HTTP/1.1\r\nHost: t\r\nX: " + std::string(600, 'a') + "\r\n\r\n",
         "HTTP/1.1 431 Request Header Fields Too Large", "header_too_large"},
        {"PUT / HTTP/1.1\r\nHost: t\r\nContent-Length: 17\r\n\r\n",
         "HTTP/1.1 413 Payload Too Large", "content_too_large"},
        {"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         "HTTP/1.1 411 Length Required", "length_required"},
        {"POST / HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n",
         "HTTP/1.1 417 Expectation Failed", "expectation_failed"},
        {"GET / HTTP/1.0\r\nHost: t\r\n\r\n", "HTTP/1.1 505 HTTP Version Not Supported",
         "version_not_supported"},
        {"BREW / HTTP/1.1\r\nHost: t\r\n\r\n", "HTTP/1.1 501 Not Implemented", "not_implemented"},
        {"GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request", "bad_request"},
        {"GET / HTTP/1.1\r\nHost t\r\n\r\n", "HTTP/1.1 400 Bad Request", "bad_request"},
    }};
    u64 rejected = 0;
    for (const Rejection& rejection : cases) {
        Client client(port());
        client.send(rejection.request);
        const std::string reply = client.read_to_end();
        EXPECT_TRUE(reply.starts_with(std::string(rejection.status_line) + "\r\n")) << reply;
        EXPECT_NE(reply.find("\r\nConnection: close\r\n"), std::string::npos) << reply;
        EXPECT_TRUE(reply.ends_with(std::format(R"({{"error":"{}"}})", rejection.code))) << reply;
        ++rejected;
        // Server đóng êm: chờ client đóng phía nó.
        client.close();
        wait_until([&] { return stats().connections() == 0; });
        EXPECT_EQ(stats().rejected, rejected);
    }
    EXPECT_EQ(handled(), 0U);
    EXPECT_EQ(stats().requests, 0U);
}

// Body quá lớn bị từ chối mà không đọc; client vẫn gửi body thì server đọc bỏ trước khi đóng, nên
// response 413 tới được client thay vì bị TCP reset xoá.
TEST_F(ServerTest, LargeBodyIsRefusedWithoutBeingRead) {
    ServerConfig config;
    config.limits.max_body_bytes = 1'024;
    start(config);
    Client client(port());
    client.send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 300000\r\n\r\n" +
                std::string(300'000, 'b'));
    const std::string reply = client.read_to_end();
    EXPECT_TRUE(reply.starts_with("HTTP/1.1 413 ")) << reply.substr(0, 64);
    EXPECT_TRUE(reply.ends_with(R"({"error":"content_too_large"})"));
    client.close();
    wait_until([&] { return stats().connections() == 0; });
    EXPECT_EQ(handled(), 0U);
}

TEST_F(ServerTest, SlowRequestGets408AndIsClosed) {
    start();
    Client client(port());
    client.send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nabc");
    wait_until([&] { return stats().reading == 1; });
    const core::Duration read_timeout = Limits{}.read_timeout;
    // Chưa tới hạn: check_deadlines không đụng gì (nó quyết định ngay khi quét).
    advance(read_timeout - core::Duration::nanoseconds(1));
    EXPECT_EQ(stats().reading, 1U);
    advance(core::Duration::nanoseconds(1));
    const std::string reply = client.read_to_end();
    EXPECT_TRUE(reply.starts_with("HTTP/1.1 408 Request Timeout\r\n")) << reply;
    EXPECT_NE(reply.find("\r\nConnection: close\r\n"), std::string::npos) << reply;
    EXPECT_TRUE(reply.ends_with(R"({"error":"request_timeout"})")) << reply;
    client.close();
    wait_until([&] { return stats().connections() == 0; });
    EXPECT_EQ(stats().timed_out, 1U);
    EXPECT_EQ(handled(), 0U);
}

TEST_F(ServerTest, IdleConnectionIsClosedWithoutAResponse) {
    start();
    Client client(port());
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_NE(client.read().result_int(), 0U);
    wait_until([&] { return stats().waiting == 1; });
    advance(Limits{}.idle_timeout);
    EXPECT_EQ(client.read_to_end(), "");
    wait_until([&] { return stats().connections() == 0; });
    EXPECT_EQ(stats().timed_out, 1U);
}

// Việc quét hạn theo chu kỳ của server đóng kết nối quá hạn mà test không gọi check_deadlines: đồng
// hồ giả đã qua hạn, bộ hẹn giờ thật chỉ kích việc quét (server.hpp).
TEST_F(ServerTest, PeriodicSweepClosesExpiredConnections) {
    ServerConfig config;
    config.check_interval = core::Duration::milliseconds(1);
    start(config, true);
    Client client(port());
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_NE(client.read().result_int(), 0U);
    wait_until([&] { return stats().waiting == 1; });
    clock().advance(Limits{}.idle_timeout);
    EXPECT_EQ(client.read_to_end(), "");
    wait_until([&] { return stats().connections() == 0; });
    EXPECT_EQ(stats().timed_out, 1U);
}

// Client đóng giữa chừng một request: server đóng theo, không trả lỗi, không tính quá hạn.
TEST_F(ServerTest, ClientClosingMidRequestIsNotAnError) {
    start();
    Client client(port());
    client.send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nab");
    wait_until([&] { return stats().reading == 1; });
    client.close();
    wait_until([&] { return stats().connections() == 0; });
    EXPECT_EQ(stats().rejected, 0U);
    EXPECT_EQ(stats().timed_out, 0U);
    EXPECT_EQ(handled(), 0U);
}

// Client không đọc response lớn thì server không giữ kết nối mãi, dù lượt ghi kẹt hay xong. Kẹt khi
// bộ đệm của hai đầu không chứa hết 32 MiB (Linux ở máy dev: tcp_wmem tối đa 4 MiB, bộ đệm nhận của
// client 64 KiB): write_timeout đóng kết nối. Xong khi chúng chứa hết (Windows ở CI run
// 36372839028: client nhận đủ 33 554 579 byte): kết nối về chờ request, và idle_timeout đóng nó.
// Đường nào cũng là đúng một lần quá hạn.
TEST_F(ServerTest, ClientThatStopsReadingIsClosedAfterTheWriteTimeout) {
    set_handler([](const Request&) {
        Response response;
        response.content_type = "application/octet-stream";
        response.body = std::string(usize{32} * 1024 * 1024, 'x');
        return response;
    });
    start();
    Client client(port(), 64 * 1024);
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    // Đọc handled() trước stats(): handler đã chạy thì waiting == 1 chỉ có thể là sau lượt ghi.
    // Nhận cả hai pha vì lượt ghi xong nhanh có thể lọt giữa hai lần hỏi.
    wait_until([&] {
        if (handled() != 1) {
            return false;
        }
        const ServerStats now = stats();
        return now.writing == 1 || now.waiting == 1;
    });
    advance(Limits{}.write_timeout);
    wait_until([&] {
        const ServerStats now = stats();
        return now.connections() == 0 || now.waiting == 1;
    });
    if (stats().connections() != 0) {
        advance(Limits{}.idle_timeout);
        wait_until([&] { return stats().connections() == 0; });
    }
    EXPECT_EQ(stats().timed_out, 1U);
    static_cast<void>(client.read_to_end());
}

// Sau response kèm Connection: close, client không đóng phía nó: server chỉ chờ linger_timeout.
TEST_F(ServerTest, LingerEndsAfterTheLingerTimeout) {
    start();
    Client client(port());
    client.send("GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    const Reply reply = client.read();
    ASSERT_NE(reply.result_int(), 0U);
    EXPECT_FALSE(reply.keep_alive());
    wait_until([&] { return stats().closing == 1; });
    advance(Limits{}.linger_timeout);
    wait_until([&] { return stats().connections() == 0; });
    EXPECT_EQ(client.read_to_end(), "");
    // Chờ client đóng không phải quá hạn của request.
    EXPECT_EQ(stats().timed_out, 0U);
}

TEST_F(ServerTest, ConnectionClosesAfterMaxRequests) {
    ServerConfig config;
    config.limits.max_requests_per_connection = 2;
    start(config);
    Client client(port());
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply first = client.read();
    ASSERT_NE(first.result_int(), 0U);
    EXPECT_TRUE(first.keep_alive());
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply second = client.read();
    ASSERT_NE(second.result_int(), 0U);
    EXPECT_FALSE(second.keep_alive());
    EXPECT_EQ(client.read_to_end(), "");
}

TEST_F(ServerTest, HandlerCanCloseAndSetHeaders) {
    set_handler([](const Request& request) {
        Response response = error_response(Status::Unauthorized, "unauthenticated");
        response.headers.push_back({.name = "WWW-Authenticate", .value = "Bearer"});
        response.close = request.path() == "/close";
        return response;
    });
    start();
    Client client(port());
    client.send("GET /keep HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply kept = client.read();
    ASSERT_NE(kept.result_int(), 0U);
    EXPECT_EQ(kept.result_int(), 401U);
    EXPECT_EQ(field(kept, beast_http::field::www_authenticate), "Bearer");
    EXPECT_TRUE(kept.keep_alive());
    client.send("GET /close HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply closed = client.read();
    ASSERT_NE(closed.result_int(), 0U);
    EXPECT_FALSE(closed.keep_alive());
    EXPECT_EQ(client.read_to_end(), "");
}

// Header sai từ handler (ví dụ giá trị lấy từ dữ liệu ngoài có CR, LF) thành 500, không bao giờ
// được ghi ra.
TEST_F(ServerTest, InvalidHandlerHeaderBecomes500) {
    set_handler([](const Request&) {
        Response response;
        response.headers.push_back({.name = "X-Echo", .value = "a\r\nSet-Cookie: s=1"});
        return response;
    });
    start();
    Client client(port());
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply reply = client.read();
    ASSERT_NE(reply.result_int(), 0U);
    EXPECT_EQ(reply.result_int(), 500U);
    EXPECT_EQ(reply.body(), R"({"error":"internal"})");
    EXPECT_EQ(reply.find(beast_http::field::set_cookie), reply.end());
    EXPECT_TRUE(reply.keep_alive());
}

// Hàng đợi tới worker đầy thì 503 ngay, kèm Retry-After; kết nối vẫn dùng tiếp được.
TEST_F(ServerTest, FullHandlerQueueAnswers503) {
    Gate gate;
    set_handler([&gate](const Request& request) {
        if (request.path() == "/block") {
            gate.wait();
        }
        Response response;
        response.body = std::string(request.path());
        return response;
    });
    ServerConfig config;
    config.workers = 1;
    config.queue_capacity = 1;
    start(config);
    Client running(port());
    running.send("GET /block HTTP/1.1\r\nHost: t\r\n\r\n");
    gate.wait_for_waiters(1);
    Client queued(port());
    queued.send("GET /block HTTP/1.1\r\nHost: t\r\n\r\n");
    wait_until([&] { return stats().handling == 2; });

    Client refused(port());
    refused.send("GET /free HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply busy = refused.read();
    ASSERT_NE(busy.result_int(), 0U);
    EXPECT_EQ(busy.result_int(), 503U);
    EXPECT_EQ(field(busy, beast_http::field::retry_after), "1");
    EXPECT_EQ(busy.body(), R"({"error":"overloaded"})");
    EXPECT_TRUE(busy.keep_alive());

    gate.open();
    for (Client* client : {&running, &queued}) {
        const Reply reply = client->read();
        ASSERT_NE(reply.result_int(), 0U);
        EXPECT_EQ(reply.body(), "/block");
    }
    refused.send("GET /free HTTP/1.1\r\nHost: t\r\n\r\n");
    const Reply later = refused.read();
    ASSERT_NE(later.result_int(), 0U);
    EXPECT_EQ(later.body(), "/free");
    EXPECT_EQ(stats().rejected, 1U);
}

// Đủ max_connections thì server ngừng nhận; kết nối mới chờ trong hàng listen tới khi có chỗ.
TEST_F(ServerTest, ConnectionLimitHoldsNewConnectionsBack) {
    ServerConfig config;
    config.max_connections = 1;
    start(config);
    Client first(port());
    first.send("GET /1 HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_NE(first.read().result_int(), 0U);
    Client second(port());
    ASSERT_TRUE(second.connected());
    second.send("GET /2 HTTP/1.1\r\nHost: t\r\n\r\n");
    // Thêm một vòng qua server trên kết nối thứ nhất trước khi xem số kết nối đã nhận.
    first.send("GET /1 HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_NE(first.read().result_int(), 0U);
    EXPECT_EQ(stats().accepted, 1U);
    first.close();
    const Reply reply = second.read();
    ASSERT_NE(reply.result_int(), 0U);
    EXPECT_EQ(reply.body(), R"({"method":0,"path":"/2","query":"","body":"","x":"-"})");
    EXPECT_EQ(stats().accepted, 2U);
}

TEST_F(ServerTest, StopClosesConnectionsAndIsIdempotent) {
    start();
    Client client(port());
    client.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_NE(client.read().result_int(), 0U);
    const u16 old_port = port();
    server().stop();
    EXPECT_EQ(client.read_to_end(), "");
    server().stop();
    EXPECT_EQ(stats().connections(), 0U);
    const Client late(old_port);
    EXPECT_FALSE(late.connected());
}

TEST(Server, StartRefusesBadConfiguration) {
    const core::FakeMonotonicClock clock;
    const core::FakeWallClock wall_clock;
    const Handler handler = [](const Request&) {
        return Response{};
    };
    const auto code = [&](const ServerConfig& config, const Handler& h) {
        const Result<std::unique_ptr<Server>> started = Server::start(config, h, clock, wall_clock);
        return started ? ErrorCode{} : started.error().code();
    };
    ServerConfig config;
    config.address = "localhost";
    EXPECT_EQ(code(config, handler), ErrorCode::InvalidArgument);
    config = {};
    config.workers = 0;
    EXPECT_EQ(code(config, handler), ErrorCode::InvalidArgument);
    config = {};
    config.limits.max_header_bytes = 65'536;
    EXPECT_EQ(code(config, handler), ErrorCode::InvalidArgument);
    config = {};
    config.limits.read_timeout = {};
    EXPECT_EQ(code(config, handler), ErrorCode::InvalidArgument);
    EXPECT_EQ(code(ServerConfig{}, Handler{}), ErrorCode::InvalidArgument);

    // Cổng đang có server khác nghe.
    const Result<std::unique_ptr<Server>> first = Server::start({}, handler, clock, wall_clock);
    ASSERT_TRUE(first.has_value());
    config = {};
    config.port = (*first)->port();
    EXPECT_EQ(code(config, handler), ErrorCode::Io);
    // IPv6 dạng số cũng nhận.
    config = {};
    config.address = "::1";
    const Result<std::unique_ptr<Server>> v6 = Server::start(config, handler, clock, wall_clock);
    EXPECT_TRUE(v6.has_value() || v6.error().code() == ErrorCode::Io);
}

}  // namespace
}  // namespace orion::http
