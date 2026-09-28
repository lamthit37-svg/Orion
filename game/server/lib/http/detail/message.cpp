#include "game/server/lib/http/detail/message.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/narrow.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>

namespace orion::http {
namespace detail {
namespace {

namespace beast_http = boost::beast::http;

// Beast 1.83 và 1.92 dùng boost::core::string_view; đổi tường minh để không phụ thuộc phép đổi
// ngầm của từng bản.
template <class View>
[[nodiscard]] std::string_view to_std(const View view) noexcept {
    return {view.data(), view.size()};
}

[[nodiscard]] std::optional<Method> method_of(const beast_http::verb verb) noexcept {
    switch (verb) {
        case beast_http::verb::get:
            return Method::Get;
        case beast_http::verb::head:
            return Method::Head;
        case beast_http::verb::post:
            return Method::Post;
        case beast_http::verb::put:
            return Method::Put;
        case beast_http::verb::patch:
            return Method::Patch;
        case beast_http::verb::delete_:
            return Method::Delete;
        case beast_http::verb::options:
            return Method::Options;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] bool starts_with_ascii_ci(const std::string_view text,
                                        const std::string_view lower_prefix) noexcept {
    if (text.size() < lower_prefix.size()) {
        return false;
    }
    for (usize i = 0; i < lower_prefix.size(); ++i) {
        const char c = text[i];
        const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != lower_prefix[i]) {
            return false;
        }
    }
    return true;
}

struct Target {
    std::string_view path;
    std::string_view query;
};

// Dạng origin ("/p?q") hay dạng tuyệt đối với http, https ("http://host/p?q"), mà server phải nhận
// (RFC 9112 §3.2.2); dạng tuyệt đối không có đường dẫn nghĩa là "/". Dạng khác (authority, "*") là
// nullopt.
[[nodiscard]] std::optional<Target> split_target(const std::string_view target) noexcept {
    std::string_view rest = target;
    if (!target.starts_with('/')) {
        usize scheme = 0;
        if (starts_with_ascii_ci(target, "http://")) {
            scheme = 7;
        } else if (starts_with_ascii_ci(target, "https://")) {
            scheme = 8;
        } else {
            return std::nullopt;
        }
        const std::string_view after = target.substr(scheme);
        const usize authority_end = std::min(after.find_first_of("/?"), after.size());
        if (authority_end == 0) {
            return std::nullopt;
        }
        rest = after.substr(authority_end);
    }
    const usize question = std::min(rest.find('?'), rest.size());
    const std::string_view path = rest.substr(0, question);
    const std::string_view query = question < rest.size() ? rest.substr(question + 1) : "";
    return Target{.path = path.empty() ? "/" : path, .query = query};
}

}  // namespace

bool valid_limits(const Limits& limits) noexcept {
    const core::Duration zero{};
    return limits.max_header_bytes >= kMinHeaderBytes &&
           limits.max_header_bytes <= kMaxHeaderBytes && limits.max_body_bytes <= kMaxBodyBytes &&
           limits.read_timeout > zero && limits.idle_timeout > zero &&
           limits.write_timeout > zero && limits.linger_timeout > zero &&
           limits.handler_timeout > zero && limits.max_requests_per_connection > 0;
}

void configure(RequestParser& parser, const Limits& limits) noexcept {
    ORION_ASSERT(valid_limits(limits), "http: giới hạn sai");
    // Beast áp header_limit riêng cho dòng request và cho phần header; check_header kiểm tổng.
    parser.header_limit(narrow<u32>(limits.max_header_bytes));
    parser.body_limit(limits.max_body_bytes);
}

std::optional<Status> read_error_status(const boost::system::error_code& error) noexcept {
    if (error.category() != beast_http::make_error_code(beast_http::error::need_more).category()) {
        return std::nullopt;
    }
    switch (static_cast<beast_http::error>(error.value())) {
        case beast_http::error::header_limit:
            return Status::RequestHeaderFieldsTooLarge;
        case beast_http::error::body_limit:
            return Status::ContentTooLarge;
        case beast_http::error::end_of_stream:
        case beast_http::error::partial_message:
        case beast_http::error::short_read:
        case beast_http::error::need_more:
        case beast_http::error::need_buffer:
        case beast_http::error::stale_parser:
            return std::nullopt;
        default:
            return Status::BadRequest;
    }
}

std::optional<Status> check_header(const RequestParser& parser, const usize header_bytes,
                                   const Limits& limits) noexcept {
    const BeastRequest& request = parser.get();
    if (header_bytes > limits.max_header_bytes) {
        return Status::RequestHeaderFieldsTooLarge;
    }
    if (request.version() != 11) {
        return Status::HttpVersionNotSupported;
    }
    if (!method_of(request.method())) {
        return Status::NotImplemented;
    }
    if (!split_target(to_std(request.target())) || request.count(beast_http::field::host) != 1) {
        return Status::BadRequest;
    }
    // Không đọc body chunked hay trailer (ADR 0015, quyết định 5): client phải gửi Content-Length.
    // count, không contains: basic_fields::contains có từ Beast 359 (CHANGELOG của Beast), còn
    // Boost 1.83, bản tối thiểu CMake nhận (ADR 0015, quyết định 2), mang Beast 347.
    // NOLINTNEXTLINE(readability-container-contains): xem trên.
    if (request.count(beast_http::field::transfer_encoding) != 0) {
        return Status::LengthRequired;
    }
    // NOLINTNEXTLINE(readability-container-contains): như Transfer-Encoding ở trên.
    if (request.count(beast_http::field::expect) != 0) {
        return Status::ExpectationFailed;
    }
    return std::nullopt;
}

std::unique_ptr<Message> finish(RequestParser& parser) {
    auto message = std::make_unique<Message>();
    message->request = parser.release();
    const std::optional<Method> method = method_of(message->request.method());
    const std::optional<Target> target = split_target(to_std(message->request.target()));
    ORION_ASSERT(method.has_value() && target.has_value(), "http: finish trước check_header");
    if (method && target) {
        message->method = *method;
        message->path = target->path;
        message->query = target->query;
    }
    return message;
}

namespace {

enum class Step : u8 {
    Done,
    NeedMore,
    Failed,
};

// Đưa byte từ `bytes[used..]` vào parser tới khi `done(parser)`, như async_read_header và
// async_read làm với byte đọc từ socket.
template <class Done>
[[nodiscard]] Step feed(RequestParser& parser, const std::string_view bytes, usize& used,
                        boost::system::error_code& error, Done done) {
    while (!done(parser)) {
        const std::string_view rest = bytes.substr(used);
        const usize consumed =
            parser.put(boost::asio::const_buffer(rest.data(), rest.size()), error);
        used += consumed;
        if (error == beast_http::error::need_more || (!error && consumed == 0)) {
            if (consumed == 0) {
                return Step::NeedMore;
            }
            error = {};
            continue;
        }
        if (error) {
            return Step::Failed;
        }
    }
    return Step::Done;
}

}  // namespace
}  // namespace detail

ParsedRequest::ParsedRequest() noexcept = default;
ParsedRequest::ParsedRequest(ParsedRequest&&) noexcept = default;
ParsedRequest& ParsedRequest::operator=(ParsedRequest&&) noexcept = default;
ParsedRequest::~ParsedRequest() = default;

Status ParsedRequest::status() const noexcept {
    ORION_ASSERT(state_ == State::Rejected, "http: status() của request không bị từ chối");
    return status_;
}

Request ParsedRequest::request(const core::MonoTime deadline) const noexcept {
    ORION_ASSERT(state_ == State::Complete && message_ != nullptr,
                 "http: request() của request chưa đọc đủ");
    return {*message_, deadline};
}

ParsedRequest parse_request(const std::string_view bytes, const Limits& limits) noexcept {
    ParsedRequest out;
    detail::RequestParser parser;
    detail::configure(parser, limits);
    boost::system::error_code error;
    usize used = 0;

    detail::Step step =
        detail::feed(parser, bytes, used, error,
                     [](const detail::RequestParser& p) { return p.is_header_done(); });
    if (step == detail::Step::Failed) {
        out.reject(detail::read_error_status(error).value_or(Status::BadRequest));
        return out;
    }
    if (step == detail::Step::NeedMore) {
        return out;
    }
    if (const std::optional<Status> status = detail::check_header(parser, used, limits)) {
        out.reject(*status);
        return out;
    }
    step = detail::feed(parser, bytes, used, error,
                        [](const detail::RequestParser& p) { return p.is_done(); });
    if (step == detail::Step::Failed) {
        out.reject(detail::read_error_status(error).value_or(Status::BadRequest));
        return out;
    }
    if (step == detail::Step::NeedMore) {
        return out;
    }
    out.message_ = detail::finish(parser);
    out.size_ = used;
    out.state_ = ParsedRequest::State::Complete;
    return out;
}

Method Request::method() const noexcept {
    return message_->method;
}

std::string_view Request::target() const noexcept {
    return detail::to_std(message_->request.target());
}

std::string_view Request::path() const noexcept {
    return message_->path;
}

std::string_view Request::query() const noexcept {
    return message_->query;
}

std::optional<std::string_view> Request::header(const std::string_view name) const noexcept {
    const auto found = message_->request.find(boost::beast::string_view(name.data(), name.size()));
    if (found == message_->request.end()) {
        return std::nullopt;
    }
    return detail::to_std(found->value());
}

std::string_view Request::body() const noexcept {
    return message_->request.body();
}

}  // namespace orion::http
