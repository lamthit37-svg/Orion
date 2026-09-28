#include "game/server/lib/http/detail/response.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/json.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/beast/core/string.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace orion::http {
namespace detail {
namespace {

namespace beast_http = boost::beast::http;

[[nodiscard]] boost::beast::string_view to_beast(const std::string_view text) noexcept {
    return {text.data(), text.size()};
}

[[nodiscard]] char ascii_lower(const char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equals_ascii_ci(const std::string_view text,
                                   const std::string_view lower) noexcept {
    return std::ranges::equal(text, lower, {}, ascii_lower);
}

// tchar của RFC 9110 §5.6.2.
[[nodiscard]] bool is_token_char(const char c) noexcept {
    constexpr std::string_view kSymbols = "!#$%&'*+-.^_`|~";
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           kSymbols.find(c) != std::string_view::npos;
}

[[nodiscard]] bool valid_name(const std::string_view name) noexcept {
    return !name.empty() && name.size() <= kMaxHeaderNameBytes &&
           std::ranges::all_of(name, is_token_char);
}

// Không ký tự điều khiển nào ngoài tab (RFC 9110 §5.5): CR, LF không thể lọt vào để tách thêm
// header hay response từ dữ liệu mà handler đưa vào giá trị.
[[nodiscard]] bool valid_value(const std::string_view value) noexcept {
    return value.size() <= kMaxHeaderValueBytes && std::ranges::none_of(value, [](const char c) {
               const auto byte = static_cast<u8>(c);
               return (byte < 0x20U && c != '\t') || byte == 0x7FU;
           });
}

// Header server tự quản: handler đặt chúng thì sai khung của response.
[[nodiscard]] bool reserved(const std::string_view name) noexcept {
    constexpr std::array<std::string_view, 10> kReserved{
        "connection", "content-length", "content-type",      "date",   "keep-alive", "retry-after",
        "te",         "trailer",        "transfer-encoding", "upgrade"};
    return std::ranges::any_of(
        kReserved, [name](const std::string_view r) { return equals_ascii_ci(name, r); });
}

[[nodiscard]] bool valid_headers(const Response& response) noexcept {
    return valid_value(response.content_type) &&
           std::ranges::all_of(response.headers, [](const Header& header) {
               return valid_name(header.name) && valid_value(header.value) &&
                      !reserved(header.name);
           });
}

// Giây, làm tròn lên; không tràn với mọi Duration dương.
[[nodiscard]] i64 whole_seconds_up(const core::Duration delay) noexcept {
    constexpr i64 kNanosPerSecond = 1'000'000'000;
    const i64 ns = delay.as_nanoseconds();
    return (ns / kNanosPerSecond) + (ns % kNanosPerSecond != 0 ? 1 : 0);
}

}  // namespace

std::string http_date(const core::WallTime time) {
    constexpr i64 kSecondsPerDay = 86'400;
    // 1970-01-01 là thứ năm.
    constexpr std::array<std::string_view, 7> kWeekdays{"Thu", "Fri", "Sat", "Sun",
                                                        "Mon", "Tue", "Wed"};
    constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const i64 seconds = time.unix_seconds();
    const i64 days = seconds >= 0 ? seconds / kSecondsPerDay
                                  : -((-seconds + kSecondsPerDay - 1) / kSecondsPerDay);
    const i64 in_day = seconds - (days * kSecondsPerDay);
    const core::CivilDate date = core::civil_from_days(days);
    const auto weekday = static_cast<usize>(((days % 7) + 7) % 7);
    return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT", kWeekdays.at(weekday), date.day,
                       kMonths.at(date.month - 1), date.year, in_day / 3'600, (in_day / 60) % 60,
                       in_day % 60);
}

Result<void> build(Response response, const Framing& framing, BeastResponse& out) {
    if (!valid_headers(response)) {
        return fail(ErrorCode::InvalidArgument, "http: handler đặt header sai hay header tự quản",
                    std::to_underlying(response.status));
    }
    const bool no_content = response.status == Status::NoContent;
    ORION_ASSERT(!no_content || response.body.empty(), "http: response 204 có body");
    out = BeastResponse(static_cast<beast_http::status>(std::to_underlying(response.status)), 11);
    out.set(beast_http::field::date, http_date(framing.now));
    if (!response.content_type.empty() && !no_content) {
        out.set(beast_http::field::content_type, to_beast(response.content_type));
    }
    bool cache_control = false;
    for (const Header& header : response.headers) {
        cache_control = cache_control || equals_ascii_ci(header.name, "cache-control");
        out.insert(to_beast(header.name), to_beast(header.value));
    }
    if (!cache_control) {
        out.set(beast_http::field::cache_control, "no-store");
    }
    if (response.retry_after > core::Duration{}) {
        out.set(beast_http::field::retry_after,
                std::format("{}", whole_seconds_up(response.retry_after)));
    }
    out.keep_alive(framing.keep_alive && !response.close);
    if (no_content) {
        // 204 không có body và không có Content-Length (RFC 9110 §8.6); prepare_payload của Beast
        // 1.83 vẫn đặt Content-Length: 0, nên không gọi. Body của handler bị bỏ: bản ship không có
        // ORION_ASSERT ở trên.
        out.body().clear();
        return {};
    }
    out.body() = std::move(response.body);
    out.prepare_payload();
    if (framing.head) {
        out.body().clear();
    }
    return {};
}

}  // namespace detail

Response error_response(const Status status, const std::string_view code) {
    json::Writer writer;
    writer.begin_object();
    writer.key("error");
    writer.string(code);
    writer.end_object();
    Response response;
    response.status = status;
    response.body = writer.finish();
    return response;
}

}  // namespace orion::http
