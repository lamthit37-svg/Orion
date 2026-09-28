// Fuzz parse_request của game/server/lib/http (CLAUDE.md X.4, X.14): byte của một request HTTP là
// dữ liệu từ ngoài (X.5), và parse_request đi qua đúng các hàm server dùng với socket
// (detail/message.hpp). Mọi input cho ra một request đủ, "chưa đủ byte", hay một status từ chối
// trong tập docs/formats/http.md hứa; không bao giờ UB. Với request đủ: kích thước không vượt
// input, đường dẫn bắt đầu bằng '/', body trong giới hạn, có Host, và đọc lại đúng phần đó cho cùng
// kết quả. Chạy với hai bộ giới hạn: mặc định, và nhỏ để giới hạn bị chạm thường xuyên.

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "game/server/lib/http/server.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

using orion::http::Limits;
using orion::http::ParsedRequest;
using orion::http::Status;

// Mã mà server tự trả khi từ chối một request (docs/formats/http.md, mục "Request").
[[nodiscard]] bool promised(const Status status) {
    switch (status) {
        case Status::BadRequest:
        case Status::LengthRequired:
        case Status::ContentTooLarge:
        case Status::ExpectationFailed:
        case Status::RequestHeaderFieldsTooLarge:
        case Status::NotImplemented:
        case Status::HttpVersionNotSupported:
            return true;
        default:
            return false;
    }
}

void check(const std::string_view bytes, const Limits& limits) {
    const ParsedRequest parsed = orion::http::parse_request(bytes, limits);
    switch (parsed.state()) {
        case ParsedRequest::State::Incomplete:
            return;
        case ParsedRequest::State::Rejected:
            ORION_VERIFY(promised(parsed.status()), "http: mã từ chối ngoài hợp đồng: {}",
                         static_cast<int>(parsed.status()));
            return;
        case ParsedRequest::State::Complete:
            break;
    }
    ORION_VERIFY(parsed.size() > 0 && parsed.size() <= bytes.size(), "http: kích thước sai");
    const orion::http::Request request = parsed.request();
    ORION_VERIFY(request.path().starts_with('/'), "http: đường dẫn không bắt đầu bằng '/'");
    ORION_VERIFY(request.body().size() <= limits.max_body_bytes, "http: body vượt giới hạn");
    ORION_VERIFY(request.header("host").has_value(), "http: request đủ mà thiếu Host");
    const ParsedRequest again = orion::http::parse_request(bytes.substr(0, parsed.size()), limits);
    ORION_VERIFY(again.state() == ParsedRequest::State::Complete && again.size() == parsed.size() &&
                     again.request().target() == request.target() &&
                     again.request().body() == request.body(),
                 "http: đọc lại đúng phần của request cho kết quả khác");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes = orion::core::as_chars(std::as_bytes(std::span(data, size)));
    Limits small;
    small.max_header_bytes = 256;
    small.max_body_bytes = 64;
    for (const Limits& limits : std::array<Limits, 2>{Limits{}, small}) {
        check(bytes, limits);
    }
    return 0;
}
