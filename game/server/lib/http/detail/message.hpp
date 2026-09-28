#pragma once

// Đọc và kiểm request HTTP/1.1 (nội bộ của server_http; docs/formats/http.md, mục "Request").
// Server đọc từ socket và parse_request đọc từ bộ nhớ đi qua cùng các hàm ở đây, nên fuzz target
// http_request kiểm đúng code mà server chạy. Kiểu của Beast chỉ nằm trong detail/ và các tệp .cpp
// của module (CLAUDE.md X.2).

#include "engine/core/types.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/system/error_code.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace orion::http::detail {

using BeastRequest = boost::beast::http::request<boost::beast::http::string_body>;
using RequestParser = boost::beast::http::request_parser<boost::beast::http::string_body>;

// Một request đã đọc đủ và đã kiểm. `path`, `query` là view vào request.target(); Message không
// chép hay move được, nên view không bao giờ trỏ vào bản cũ.
struct Message {
    BeastRequest request;
    Method method = Method::Get;
    std::string_view path;
    std::string_view query;

    Message() = default;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message(Message&&) = delete;
    Message& operator=(Message&&) = delete;
    ~Message() = default;
};

// Đặt giới hạn của `limits` cho một parser mới. Tiền điều kiện: limits đã qua valid_limits.
void configure(RequestParser& parser, const Limits& limits) noexcept;

// Giới hạn mà server nhận: mọi thời hạn dương, max_header_bytes trong [kMinHeaderBytes,
// kMaxHeaderBytes], max_body_bytes không quá kMaxBodyBytes.
inline constexpr usize kMinHeaderBytes = 256;
// Beast giữ độ dài của tên và giá trị header trong 16 bit; header lớn hơn thì nó ném exception.
inline constexpr usize kMaxHeaderBytes = 65'535;
inline constexpr usize kMaxBodyBytes = usize{16} * 1024 * 1024;
[[nodiscard]] bool valid_limits(const Limits& limits) noexcept;

// Status trả client khi đọc header hay body lỗi; nullopt khi không đáng trả gì (client đã đóng, kết
// nối hỏng, thao tác bị huỷ): server chỉ đóng kết nối.
[[nodiscard]] std::optional<Status> read_error_status(
    const boost::system::error_code& error) noexcept;

// Kiểm header vừa đọc xong, dài `header_bytes` byte: nullopt khi hợp lệ, hay status để từ chối.
[[nodiscard]] std::optional<Status> check_header(const RequestParser& parser, usize header_bytes,
                                                 const Limits& limits) noexcept;

// Lấy request đã đọc đủ và đã qua check_header ra khỏi parser.
[[nodiscard]] std::unique_ptr<Message> finish(RequestParser& parser);

}  // namespace orion::http::detail
