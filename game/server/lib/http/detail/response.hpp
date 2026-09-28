#pragma once

// Dựng response HTTP/1.1 (nội bộ của server_http; docs/formats/http.md, mục "Response").

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include <string>

namespace orion::http::detail {

using BeastResponse = boost::beast::http::response<boost::beast::http::string_body>;

// Tên và giá trị header dài nhất mà một Header được mang; Beast giữ độ dài trong 16 bit.
inline constexpr usize kMaxHeaderNameBytes = 256;
inline constexpr usize kMaxHeaderValueBytes = usize{8} * 1024;

// Cách gửi một response, do request và kết nối quyết định.
struct Framing {
    // Response cho HEAD: gửi Content-Length của body mà không gửi body.
    bool head = false;
    bool keep_alive = true;
    core::WallTime now;
};

// Dựng `out` từ `response`. Lỗi InvalidArgument khi response có Header sai hay header server tự
// quản (server.hpp, Header); `out` khi đó không dùng được, bên gọi dựng một 500 thay nó.
[[nodiscard]] Result<void> build(Response response, const Framing& framing, BeastResponse& out);

// IMF-fixdate (RFC 9110 §5.6.7), ví dụ "Sun, 06 Nov 1994 08:49:37 GMT"; phần dưới giây bị bỏ.
[[nodiscard]] std::string http_date(core::WallTime time);

}  // namespace orion::http::detail
