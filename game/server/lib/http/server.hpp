#pragma once

// Server HTTP/1.1 của các dịch vụ HTTP (ARCH §4.6, §7; CLAUDE.md X.14; ADR 0015). Hợp đồng ở
// docs/formats/http.md.
//
// - Chỉ HTTP/1.1, đứng sau load balancer; TLS kết thúc ở load balancer (X.14). Không mở thẳng ra
//   internet.
// - Mỗi request có giới hạn cứng về header, body và thời gian đọc; vượt thì trả 4xx và đóng kết nối
//   (X.14). Body chunked, `Expect`, phiên bản khác 1.1 và request thiếu `Host` bị từ chối trước khi
//   đọc body.
// - Luồng IO của Asio chỉ đọc và ghi. Handler chạy trên một nhóm worker riêng, nên handler được gọi
//   truy vấn DB đồng bộ (server_db) mà không chặn luồng IO (ARCH §7). Hàng đợi tới worker có giới
//   hạn: đầy thì trả 503 ngay, không xếp hàng vô hạn.
// - Mọi hạn tính trên đồng hồ đơn điệu tiêm được, và Date của response trên đồng hồ tường tiêm
// được.
//   Bộ hẹn giờ thật duy nhất chỉ kích việc quét hạn (check_deadlines) theo chu kỳ; test tắt nó rồi
//   tự tiến đồng hồ giả và gọi check_deadlines, không chờ thật (X.4).
// - Mọi luồng xin qua engine/jobs (X.7).

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orion::http {

enum class Method : u8 {
    Get,
    Head,
    Post,
    Put,
    Patch,
    Delete,
    Options,
};

// Mã trạng thái mà server và handler dùng (RFC 9110).
enum class Status : u16 {
    Ok = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    Conflict = 409,
    LengthRequired = 411,
    ContentTooLarge = 413,
    UnsupportedMediaType = 415,
    ExpectationFailed = 417,
    UnprocessableContent = 422,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HttpVersionNotSupported = 505,
};

// Giới hạn cứng của mỗi kết nối và mỗi request (docs/formats/http.md, mục "Giới hạn").
struct Limits {
    // Dòng request cộng mọi header, tính cả dòng trống cuối.
    usize max_header_bytes = usize{8} * 1024;
    usize max_body_bytes = usize{64} * 1024;
    // Từ byte đầu tiên của một request tới khi đọc xong body.
    core::Duration read_timeout = core::Duration::seconds(10);
    // Chờ byte đầu tiên của một request, kể cả request đầu của kết nối. Dài hơn idle timeout của
    // load balancer, để load balancer luôn là bên đóng kết nối rảnh trước.
    core::Duration idle_timeout = core::Duration::seconds(75);
    // Ghi xong một response.
    core::Duration write_timeout = core::Duration::seconds(10);
    // Sau response kèm `Connection: close`, chờ client đóng phía nó (bỏ byte nó còn gửi) tối đa
    // chừng này rồi mới đóng hẳn, để response không bị TCP reset xoá trước khi client đọc.
    core::Duration linger_timeout = core::Duration::seconds(2);
    // Hạn đưa cho handler (Request::deadline), tính từ lúc đọc xong request.
    core::Duration handler_timeout = core::Duration::seconds(10);
    // Sau chừng này request, response kèm `Connection: close`.
    u32 max_requests_per_connection = 1'000;
};

namespace detail {
struct Message;
}  // namespace detail

// Một request đã đọc đủ và đã kiểm. Rẻ để chép; mọi string_view sống như nguồn của nó (với server:
// tới khi handler trả về; với ParsedRequest: như ParsedRequest).
class Request {
public:
    [[nodiscard]] Method method() const noexcept;
    // Như client gửi, ví dụ "/v1/login?x=1", hay dạng tuyệt đối "http://host/v1/login".
    [[nodiscard]] std::string_view target() const noexcept;
    // Đường dẫn, luôn bắt đầu bằng '/'; chưa giải mã %XX.
    [[nodiscard]] std::string_view path() const noexcept;
    // Phần sau '?', không kèm '?'; rỗng khi không có.
    [[nodiscard]] std::string_view query() const noexcept;
    // Giá trị của header đầu tiên tên `name`, không phân biệt hoa thường.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view body() const noexcept;
    // Hạn của request trên đồng hồ của server: truyền nguyên cho mọi lời gọi ra ngoài (X.14).
    [[nodiscard]] core::MonoTime deadline() const noexcept { return deadline_; }

    Request(const detail::Message& message, core::MonoTime deadline) noexcept
        : message_(&message), deadline_(deadline) {}

private:
    const detail::Message* message_;
    core::MonoTime deadline_;
};

// Một header thêm vào response. Tên là token (RFC 9110 §5.1), thường là literal; giá trị không có
// ký tự điều khiển nào ngoài tab. Header sai, hay header server tự quản (Connection,
// Content-Length, Content-Type, Date, Retry-After, Transfer-Encoding), thì server trả 500 thay cho
// response đó và log lỗi: không bao giờ ghi ra header sai.
struct Header {
    std::string_view name;
    std::string value;
};

// Mọi trường có giá trị mặc định để handler khởi tạo chỉ định bỏ được trường không dùng: thiếu
// `{}`, clang cảnh báo trường bị bỏ (-Wmissing-designated-field-initializers), nên `{}` không thừa
// như clang-tidy nghĩ.
struct Response {
    Status status = Status::Ok;
    // Body; rỗng với 204. Với HEAD, server chỉ gửi Content-Length của nó.
    std::string body{};  // NOLINT(readability-redundant-member-init): xem trên
    // Chỉ literal hay chuỗi sống suốt chương trình; rỗng thì không gửi Content-Type.
    std::string_view content_type = "application/json";
    // Không có Cache-Control thì server thêm `Cache-Control: no-store`.
    std::vector<Header> headers{};  // NOLINT(readability-redundant-member-init): như trên
    // Khác 0: header Retry-After tính bằng giây, làm tròn lên (429, 503).
    core::Duration retry_after{};  // NOLINT(readability-redundant-member-init): như trên
    // Đóng kết nối sau response.
    bool close = false;
};

// Response lỗi dạng JSON {"error": code} (docs/formats/http.md, mục "Lỗi"). `code` là mã ngắn
// snake_case, ví dụ "invalid_field".
[[nodiscard]] Response error_response(Status status, std::string_view code);

// Gọi trên một worker; nhiều worker gọi cùng lúc, nên handler phải an toàn giữa các luồng. Handler
// phải trả về trước Request::deadline cộng một khoảng nhỏ: server không ngắt được handler.
using Handler = std::function<Response(const Request&)>;

// Kết quả đọc một request từ bộ nhớ bằng đúng các bước server dùng với socket: cho fuzz target và
// cho test của các dịch vụ dựng request mà không cần socket.
class ParsedRequest {
public:
    enum class State : u8 {
        // Đủ một request hợp lệ.
        Complete,
        // Chưa đủ byte; server sẽ đọc tiếp tới hạn.
        Incomplete,
        // Bị từ chối: server trả status() rồi đóng kết nối.
        Rejected,
    };

    ParsedRequest(ParsedRequest&&) noexcept;
    ParsedRequest& operator=(ParsedRequest&&) noexcept;
    ParsedRequest(const ParsedRequest&) = delete;
    ParsedRequest& operator=(const ParsedRequest&) = delete;
    ~ParsedRequest();

    [[nodiscard]] State state() const noexcept { return state_; }
    // Tiền điều kiện: state() == Rejected.
    [[nodiscard]] Status status() const noexcept;
    // Tiền điều kiện: state() == Complete. Request sống như ParsedRequest.
    [[nodiscard]] Request request(core::MonoTime deadline = {}) const noexcept;
    // Complete: số byte của request, header cộng body; byte sau đó thuộc request kế tiếp.
    [[nodiscard]] usize size() const noexcept { return size_; }

private:
    friend ParsedRequest parse_request(std::string_view bytes, const Limits& limits) noexcept;
    ParsedRequest() noexcept;
    void reject(Status status) noexcept {
        state_ = State::Rejected;
        status_ = status;
    }

    std::unique_ptr<detail::Message> message_;
    usize size_ = 0;
    State state_ = State::Incomplete;
    Status status_ = Status::BadRequest;
};

// Đọc request đầu tiên trong `bytes` (docs/formats/http.md, mục "Request").
[[nodiscard]] ParsedRequest parse_request(std::string_view bytes, const Limits& limits) noexcept;

struct ServerConfig {
    // Địa chỉ IPv4 hay IPv6 dạng số: không tra DNS.
    std::string_view address = "127.0.0.1";
    // 0: hệ điều hành chọn; đọc lại bằng Server::port().
    u16 port = 0;
    u32 io_threads = 1;
    u32 workers = 4;
    // Request đã đọc xong chờ worker; đầy thì 503.
    u32 queue_capacity = 64;
    // Kết nối mở cùng lúc; tới đó thì server ngừng nhận kết nối mới (chúng chờ trong hàng listen
    // của hệ điều hành) cho tới khi có kết nối đóng.
    u32 max_connections = 1'024;
    Limits limits{};
    // Chu kỳ thật của việc quét hạn; 0 thì không tự quét (test tự gọi check_deadlines).
    core::Duration check_interval = core::Duration::milliseconds(100);
};

// Số đo của server, cho metric và test. Mỗi trường đọc riêng, nên các trường có thể lệch nhau một
// chút khi server đang chạy. Thấy một kết nối ở một pha thì cũng thấy hạn của pha đó: test tiến
// đồng hồ giả dựa vào điều này.
struct ServerStats {
    // Kết nối đang mở, theo pha.
    usize waiting = 0;   // chờ request
    usize reading = 0;   // đang đọc request
    usize handling = 0;  // chờ worker hay trong handler
    usize writing = 0;   // ghi response
    usize closing = 0;   // đóng êm sau response cuối (Limits::linger_timeout)
    // Đếm dồn từ lúc start.
    u64 accepted = 0;   // kết nối đã nhận
    u64 requests = 0;   // request đã đưa handler
    u64 rejected = 0;   // response lỗi server tự trả, không qua handler
    u64 timed_out = 0;  // kết nối bị đóng vì quá hạn

    [[nodiscard]] usize connections() const noexcept {
        return waiting + reading + handling + writing + closing;
    }
};

class Server {
public:
    // Mở cổng và chạy luồng IO, worker. Hai đồng hồ phải sống lâu hơn Server. Lỗi: InvalidArgument
    // (địa chỉ sai, số luồng, sức chứa hay giới hạn bằng 0), Io (mở, bind, listen; detail là mã lỗi
    // của hệ điều hành), ResourceExhausted (không tạo được luồng).
    [[nodiscard]] static Result<std::unique_ptr<Server>> start(const ServerConfig& config,
                                                               Handler handler,
                                                               const core::MonotonicClock& clock,
                                                               const core::WallClock& wall_clock);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    // stop().
    ~Server();

    [[nodiscard]] u16 port() const noexcept;
    [[nodiscard]] ServerStats stats() const noexcept;
    // Đóng các kết nối có hạn đã qua trên đồng hồ của server: đang đọc request thì trả 408 rồi
    // đóng, đang chờ, ghi hay đóng êm thì đóng. Gọi từ luồng nào cũng được.
    void check_deadlines() noexcept;
    // Ngừng nhận kết nối, đóng mọi kết nối, chờ handler đang chạy trả về, rồi join mọi luồng. Gọi
    // nhiều lần vô hại. Không gọi từ trong handler.
    void stop() noexcept;

    // Chỉ Server::start dựng Server: Impl chỉ định nghĩa trong server.cpp.
    class Impl;
    explicit Server(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace orion::http
