// Dựng response (detail/response.hpp) và error_response: đúng byte ghi ra mạng
// (docs/formats/http.md, mục "Response").

#include "game/server/lib/http/detail/response.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/beast/http/write.hpp>
#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace orion::http {
namespace {

constexpr core::WallTime kNow = core::WallTime::from_unix_seconds(784'111'777);

// Dạng chữ của response: dòng trạng thái, header theo thứ tự, body. Byte thật trên dây do test
// loopback (server_test.cpp) kiểm; ở đây không dùng operator<< của Beast, vì đường đó của Beast
// 1.83 dùng std::aligned_storage mà libstdc++ 14 báo deprecated ở C++23.
[[nodiscard]] std::string wire(const detail::BeastResponse& response) {
    const auto text = [](const auto view) {
        return std::string_view(view.data(), view.size());
    };
    std::string out =
        std::format("HTTP/1.1 {} {}\r\n", response.result_int(), text(response.reason()));
    for (const auto& field : response) {
        out += std::format("{}: {}\r\n", text(field.name_string()), text(field.value()));
    }
    out += "\r\n";
    out += response.body();
    return out;
}

// Dựng response; chuỗi rỗng khi build từ chối.
[[nodiscard]] std::string built(Response response, const detail::Framing& framing) {
    detail::BeastResponse out;
    if (!detail::build(std::move(response), framing, out)) {
        return {};
    }
    return wire(out);
}

[[nodiscard]] Response json_body(std::string body) {
    Response response;
    response.body = std::move(body);
    return response;
}

TEST(HttpDate, IsImfFixdateInUtc) {
    const auto date = [](const i64 seconds) {
        return detail::http_date(core::WallTime::from_unix_seconds(seconds));
    };
    // Giá trị đối chiếu từ email.utils.formatdate(t, usegmt=True) của Python.
    EXPECT_EQ(date(784'111'777), "Sun, 06 Nov 1994 08:49:37 GMT");
    EXPECT_EQ(date(0), "Thu, 01 Jan 1970 00:00:00 GMT");
    EXPECT_EQ(date(951'782'400), "Tue, 29 Feb 2000 00:00:00 GMT");
    EXPECT_EQ(date(-1), "Wed, 31 Dec 1969 23:59:59 GMT");
    EXPECT_EQ(date(2'147'483'648), "Tue, 19 Jan 2038 03:14:08 GMT");
    EXPECT_EQ(date(4'102'444'799), "Thu, 31 Dec 2099 23:59:59 GMT");
    // Phần dưới giây bị bỏ, không làm tròn.
    EXPECT_EQ(detail::http_date(core::WallTime::from_unix_microseconds(784'111'777'999'999)),
              "Sun, 06 Nov 1994 08:49:37 GMT");
}

TEST(BuildResponse, DefaultsForAKeepAliveResponse) {
    EXPECT_EQ(built(json_body(R"({"ok":true})"), {.head = false, .keep_alive = true, .now = kNow}),
              "HTTP/1.1 200 OK\r\n"
              "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
              "Content-Type: application/json\r\n"
              "Cache-Control: no-store\r\n"
              "Content-Length: 11\r\n"
              "\r\n"
              R"({"ok":true})");
}

TEST(BuildResponse, CloseHeadAndNoContent) {
    const std::string closing =
        built(json_body("{}"), {.head = false, .keep_alive = false, .now = kNow});
    EXPECT_NE(closing.find("\r\nConnection: close\r\n"), std::string::npos) << closing;

    Response asked_to_close = json_body("{}");
    asked_to_close.close = true;
    EXPECT_NE(built(std::move(asked_to_close), {.head = false, .keep_alive = true, .now = kNow})
                  .find("\r\nConnection: close\r\n"),
              std::string::npos);

    // HEAD: Content-Length của body mà không có body.
    const std::string head =
        built(json_body("hello"), {.head = true, .keep_alive = true, .now = kNow});
    EXPECT_TRUE(head.ends_with("Content-Length: 5\r\n\r\n")) << head;

    Response none;
    none.status = Status::NoContent;
    const std::string no_content =
        built(std::move(none), {.head = false, .keep_alive = true, .now = kNow});
    EXPECT_EQ(no_content,
              "HTTP/1.1 204 No Content\r\n"
              "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
              "Cache-Control: no-store\r\n"
              "\r\n");
}

TEST(BuildResponse, HandlerHeadersAndRetryAfter) {
    Response response = json_body("{}");
    response.status = Status::Created;
    response.content_type = "application/problem+json";
    response.headers.push_back({.name = "Location", .value = "/v1/items/42"});
    response.headers.push_back({.name = "cache-control", .value = "max-age=60"});
    response.retry_after = core::Duration::milliseconds(1'500);
    EXPECT_EQ(built(std::move(response), {.head = false, .keep_alive = true, .now = kNow}),
              "HTTP/1.1 201 Created\r\n"
              "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
              "Content-Type: application/problem+json\r\n"
              "Location: /v1/items/42\r\n"
              "cache-control: max-age=60\r\n"
              "Retry-After: 2\r\n"
              "Content-Length: 2\r\n"
              "\r\n"
              "{}");

    const auto retry_after = [](const core::Duration delay) {
        Response slow;
        slow.status = Status::ServiceUnavailable;
        slow.retry_after = delay;
        const std::string text =
            built(std::move(slow), {.head = false, .keep_alive = true, .now = kNow});
        const usize at = text.find("Retry-After: ");
        return at == std::string::npos ? std::string{}
                                       : text.substr(at + 13, text.find('\r', at) - at - 13);
    };
    EXPECT_EQ(retry_after(core::Duration::seconds(1)), "1");
    EXPECT_EQ(retry_after(core::Duration::nanoseconds(1)), "1");
    EXPECT_EQ(retry_after(core::Duration::seconds(120)), "120");
    EXPECT_EQ(retry_after(core::Duration{}), "");
}

// Header sai hay header server tự quản bị từ chối, để không bao giờ ghi ra CR, LF hay khung sai.
TEST(BuildResponse, RefusesBadOrReservedHeaders) {
    const auto accepted = [](const std::string_view name, const std::string& value) {
        Response response = json_body("{}");
        response.headers.push_back({.name = name, .value = value});
        detail::BeastResponse out;
        return detail::build(std::move(response), {}, out).has_value();
    };
    EXPECT_TRUE(accepted("X-Ok", "a b\tc é"));
    EXPECT_FALSE(accepted("", "x"));
    EXPECT_FALSE(accepted("X Bad", "x"));
    EXPECT_FALSE(accepted("X:Bad", "x"));
    EXPECT_FALSE(accepted("X-Split", "a\r\nSet-Cookie: s=1"));
    EXPECT_FALSE(accepted("X-Nul", std::string("a\0b", 3)));
    EXPECT_FALSE(accepted("X-Del", "a\x7f"));
    EXPECT_TRUE(accepted(std::string(detail::kMaxHeaderNameBytes, 'n'), "x"));
    EXPECT_FALSE(accepted(std::string(detail::kMaxHeaderNameBytes + 1, 'n'), "x"));
    EXPECT_TRUE(accepted("X-Long", std::string(detail::kMaxHeaderValueBytes, 'v')));
    EXPECT_FALSE(accepted("X-Long", std::string(detail::kMaxHeaderValueBytes + 1, 'v')));
    for (const std::string_view reserved :
         {"Connection", "content-length", "Content-Type", "DATE", "Keep-Alive", "Retry-After", "TE",
          "Trailer", "Transfer-Encoding", "Upgrade"}) {
        EXPECT_FALSE(accepted(reserved, "x")) << reserved;
    }
    Response bad_type = json_body("{}");
    bad_type.content_type = "text/plain\r\nX: y";
    detail::BeastResponse out;
    EXPECT_EQ(detail::build(std::move(bad_type), {}, out).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(ErrorResponse, IsJsonWithTheCode) {
    const Response response = error_response(Status::Conflict, "item_taken");
    EXPECT_EQ(response.status, Status::Conflict);
    EXPECT_EQ(response.body, R"({"error":"item_taken"})");
    EXPECT_EQ(response.content_type, "application/json");
    EXPECT_FALSE(response.close);
}

}  // namespace
}  // namespace orion::http
