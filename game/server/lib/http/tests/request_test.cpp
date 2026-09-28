// parse_request đi qua đúng các hàm server dùng với socket (detail/message.hpp), nên mọi luật của
// docs/formats/http.md, mục "Request", được kiểm ở đây không cần mạng.

#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/server.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace orion::http {
namespace {

using State = ParsedRequest::State;

[[nodiscard]] std::optional<Status> rejection(const std::string_view bytes,
                                              const Limits& limits = {}) {
    const ParsedRequest parsed = parse_request(bytes, limits);
    if (parsed.state() != State::Rejected) {
        return std::nullopt;
    }
    return parsed.status();
}

TEST(ParseRequest, ReadsAFullRequest) {
    const std::string bytes =
        "POST /v1/login?lang=vi&x=1 HTTP/1.1\r\nHost: auth\r\nContent-Type: application/json\r\n"
        "X-Trace: a\r\nx-trace: b\r\nContent-Length: 10\r\n\r\n{\"a\":\"é\"}";
    const ParsedRequest parsed = parse_request(bytes, {});
    ASSERT_EQ(parsed.state(), State::Complete);
    EXPECT_EQ(parsed.size(), bytes.size());
    const Request request = parsed.request(core::MonoTime::from_nanoseconds(42));
    EXPECT_EQ(request.method(), Method::Post);
    EXPECT_EQ(request.target(), "/v1/login?lang=vi&x=1");
    EXPECT_EQ(request.path(), "/v1/login");
    EXPECT_EQ(request.query(), "lang=vi&x=1");
    EXPECT_EQ(request.header("content-type"), std::string_view("application/json"));
    EXPECT_EQ(request.header("X-TRACE"), std::string_view("a"));
    EXPECT_FALSE(request.header("missing").has_value());
    EXPECT_EQ(request.body(), "{\"a\":\"é\"}");
    EXPECT_EQ(request.deadline(), core::MonoTime::from_nanoseconds(42));
}

// ParsedRequest chuyển được (dựng và gán); request của nó đi theo.
TEST(ParseRequest, ParsedRequestMoves) {
    ParsedRequest parsed = parse_request("GET /a HTTP/1.1\r\nHost: x\r\n\r\n", {});
    ParsedRequest moved = std::move(parsed);
    ParsedRequest assigned = parse_request("", {});
    EXPECT_EQ(assigned.state(), State::Incomplete);
    assigned = std::move(moved);
    ASSERT_EQ(assigned.state(), State::Complete);
    EXPECT_EQ(assigned.request().path(), "/a");
}

struct MethodCase {
    std::string_view name;
    Method method;
};

TEST(ParseRequest, EveryMethodAndNoBodyWithoutContentLength) {
    constexpr std::array<MethodCase, 7> kCases{{{"GET", Method::Get},
                                                {"HEAD", Method::Head},
                                                {"POST", Method::Post},
                                                {"PUT", Method::Put},
                                                {"PATCH", Method::Patch},
                                                {"DELETE", Method::Delete},
                                                {"OPTIONS", Method::Options}}};
    for (const auto& [name, method] : kCases) {
        const std::string bytes = std::string(name) + " / HTTP/1.1\r\nHost: x\r\n\r\n";
        const ParsedRequest parsed = parse_request(bytes, {});
        ASSERT_EQ(parsed.state(), State::Complete) << name;
        EXPECT_EQ(parsed.request().method(), method) << name;
        EXPECT_TRUE(parsed.request().body().empty()) << name;
        EXPECT_EQ(parsed.request().query(), "") << name;
    }
}

TEST(ParseRequest, OnlyTheFirstOfPipelinedRequestsIsRead) {
    const std::string first = "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi";
    const ParsedRequest parsed = parse_request(first + "GET /b HTTP/1.1\r\nHost: x\r\n\r\n", {});
    ASSERT_EQ(parsed.state(), State::Complete);
    EXPECT_EQ(parsed.size(), first.size());
    EXPECT_EQ(parsed.request().path(), "/a");
    EXPECT_EQ(parsed.request().body(), "hi");
}

// Dạng tuyệt đối phải được nhận (RFC 9112 §3.2.2); dạng authority và "*" thì không.
TEST(ParseRequest, TargetForms) {
    const auto path_query = [](const std::string_view target) -> std::optional<std::string> {
        const ParsedRequest parsed =
            parse_request("GET " + std::string(target) + " HTTP/1.1\r\nHost: x\r\n\r\n", {});
        if (parsed.state() != State::Complete) {
            return std::nullopt;
        }
        return std::string(parsed.request().path()) + "|" + std::string(parsed.request().query());
    };
    EXPECT_EQ(path_query("/"), "/|");
    EXPECT_EQ(path_query("/a/b?"), "/a/b|");
    EXPECT_EQ(path_query("/a?x=1?y"), "/a|x=1?y");
    EXPECT_EQ(path_query("http://auth.example/v1/x?y=1"), "/v1/x|y=1");
    EXPECT_EQ(path_query("HTTPS://auth.example"), "/|");
    EXPECT_EQ(path_query("http://auth.example?q"), "/|q");
    EXPECT_EQ(path_query("http:///x"), std::nullopt);
    EXPECT_EQ(path_query("ftp://auth.example/x"), std::nullopt);
    EXPECT_EQ(path_query("auth.example:443"), std::nullopt);
    EXPECT_EQ(path_query("*"), std::nullopt);
    EXPECT_EQ(rejection("OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n"), Status::BadRequest);
}

TEST(ParseRequest, IncompleteHeaderAndBody) {
    EXPECT_EQ(parse_request("", {}).state(), State::Incomplete);
    EXPECT_EQ(parse_request("GET / HT", {}).state(), State::Incomplete);
    EXPECT_EQ(parse_request("GET / HTTP/1.1\r\nHost: x\r\n", {}).state(), State::Incomplete);
    EXPECT_EQ(
        parse_request("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nabc", {}).state(),
        State::Incomplete);
}

// Tổng dòng request và header, tính cả dòng trống cuối, tối đa max_header_bytes; Beast chỉ giới
// hạn từng phần, check_header giới hạn tổng.
TEST(ParseRequest, HeaderLimitCoversTheWholeHeader) {
    Limits limits;
    limits.max_header_bytes = 1'024;
    const std::string start = "GET / HTTP/1.1\r\nHost: x\r\nX-Pad: ";
    const auto request_of_size = [&](const usize total) {
        return start + std::string(total - start.size() - 4, 'a') + "\r\n\r\n";
    };
    EXPECT_EQ(parse_request(request_of_size(1'024), limits).state(), State::Complete);
    EXPECT_EQ(rejection(request_of_size(1'025), limits), Status::RequestHeaderFieldsTooLarge);
    EXPECT_EQ(rejection(request_of_size(4'000), limits), Status::RequestHeaderFieldsTooLarge);
    // Dòng request dài và phần header dài, mỗi phần dưới giới hạn nhưng tổng thì vượt.
    const std::string long_target = "/" + std::string(600, 'p');
    const std::string split = "GET " + long_target +
                              " HTTP/1.1\r\nHost: x\r\nX-Pad: " + std::string(600, 'a') +
                              "\r\n\r\n";
    EXPECT_EQ(rejection(split, limits), Status::RequestHeaderFieldsTooLarge);
    // Chưa đủ header mà đã quá giới hạn: từ chối ngay, không chờ thêm byte.
    EXPECT_EQ(rejection("GET /" + std::string(2'000, 'p'), limits),
              Status::RequestHeaderFieldsTooLarge);
}

// Content-Length lớn hơn giới hạn bị từ chối ngay khi đọc xong header, trước khi đọc body.
TEST(ParseRequest, BodyLimitIsCheckedBeforeReadingTheBody) {
    Limits limits;
    limits.max_body_bytes = 10;
    EXPECT_EQ(
        parse_request("PUT / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n0123456789", limits)
            .state(),
        State::Complete);
    EXPECT_EQ(rejection("PUT / HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\n", limits),
              Status::ContentTooLarge);
    EXPECT_EQ(rejection("PUT / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n",
                        limits),
              Status::BadRequest);
}

TEST(ParseRequest, RefusesWhatTheServerDoesNotSupport) {
    EXPECT_EQ(rejection("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "3\r\nabc\r\n0\r\n\r\n"),
              Status::LengthRequired);
    EXPECT_EQ(rejection("POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n"
                        "Content-Length: 1\r\n\r\n"),
              Status::ExpectationFailed);
    EXPECT_EQ(rejection("GET / HTTP/1.0\r\nHost: x\r\n\r\n"), Status::HttpVersionNotSupported);
    // Beast chỉ đọc được dòng request của HTTP/1.0 và 1.1; phiên bản khác là dòng request sai.
    EXPECT_EQ(rejection("GET / HTTP/2.0\r\nHost: x\r\n\r\n"), Status::BadRequest);
    EXPECT_EQ(rejection("BREW / HTTP/1.1\r\nHost: x\r\n\r\n"), Status::NotImplemented);
    EXPECT_EQ(rejection("TRACE / HTTP/1.1\r\nHost: x\r\n\r\n"), Status::NotImplemented);
    EXPECT_EQ(rejection("CONNECT x:443 HTTP/1.1\r\nHost: x\r\n\r\n"), Status::NotImplemented);
}

TEST(ParseRequest, MalformedRequestsAreBadRequests) {
    for (const std::string_view bad : {
             std::string_view{"GET / HTTP/1.1\r\n\r\n"},                        // thiếu Host
             std::string_view{"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"},  // hai Host
             std::string_view{"GET  / HTTP/1.1\r\nHost: x\r\n\r\n"},
             std::string_view{"GET / HTTP/1.1\nHost: x\n\n"},
             std::string_view{"GET / HTTP/1.1\r\nHost : x\r\n\r\n"},
             std::string_view{"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
                              "Content-Length: 2\r\n\r\n"},
             std::string_view{"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n"},
             std::string_view{"G\x01T / HTTP/1.1\r\nHost: x\r\n\r\n"},
             std::string_view{"GET /a b HTTP/1.1\r\nHost: x\r\n\r\n"},
         }) {
        EXPECT_EQ(rejection(bad), Status::BadRequest) << bad;
    }
}

// obs-fold (dòng header tiếp nối bằng khoảng trắng) được thay bằng khoảng trắng, như RFC 9112 §5.2
// cho phép thay vì từ chối.
TEST(ParseRequest, ObsFoldBecomesSpace) {
    const ParsedRequest parsed =
        parse_request("GET / HTTP/1.1\r\nHost: x\r\nX-Fold: a\r\n b\r\n\r\n", {});
    ASSERT_EQ(parsed.state(), State::Complete);
    const std::string_view folded = parsed.request().header("x-fold").value_or("");
    EXPECT_EQ(folded.find_first_of("\r\n"), std::string_view::npos);
    EXPECT_TRUE(folded.starts_with("a") && folded.ends_with("b")) << folded;
}

// Nhiều lỗi cùng lúc: thứ tự kiểm cố định (docs/formats/http.md).
TEST(ParseRequest, RuleOrderIsFixed) {
    EXPECT_EQ(rejection("BREW / HTTP/1.0\r\n\r\n"), Status::HttpVersionNotSupported);
    EXPECT_EQ(rejection("BREW * HTTP/1.1\r\n\r\n"), Status::NotImplemented);
    EXPECT_EQ(rejection("GET * HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
              Status::BadRequest);
    EXPECT_EQ(rejection("POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"),
              Status::LengthRequired);
}

}  // namespace
}  // namespace orion::http
