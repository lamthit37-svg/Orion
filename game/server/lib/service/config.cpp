// Phần chung của tệp cấu hình (docs/formats/service.md, mục "Cấu hình").

#include "engine/core/error.hpp"
#include "engine/core/log.hpp"
#include "engine/core/narrow.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/http/json.hpp"
#include "game/server/lib/http/server.hpp"
#include "game/server/lib/service/detail/secret.hpp"
#include "game/server/lib/service/service.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace orion::service {
namespace {

// Hạn tối đa của mọi trường thời gian: 10 phút.
constexpr i64 kMaxMilliseconds = 600'000;

constexpr std::array<std::string_view, 4> kCommonSections{"environment", "log", "http", "database"};

[[nodiscard]] Result<Environment> read_environment(const http::json::Object root) {
    const Result<std::string> name = config::required_text(
        root, "environment", 16, "config: environment phải là \"local\" hay \"production\"");
    if (!name) {
        return std::unexpected(name.error());
    }
    if (*name == "local") {
        return Environment::Local;
    }
    if (*name == "production") {
        return Environment::Production;
    }
    return fail(ErrorCode::InvalidArgument,
                "config: environment phải là \"local\" hay \"production\"");
}

[[nodiscard]] Result<core::LogLevel> read_log(const http::json::Object root) {
    const Result<std::optional<http::json::Object>> log =
        config::section(root, "log", "config: log phải là object");
    if (!log) {
        return std::unexpected(log.error());
    }
    if (!*log) {
        return core::LogLevel::Info;
    }
    const http::json::Object section = **log;
    constexpr std::array<std::string_view, 1> kFields{"level"};
    if (!section.expect_only(kFields)) {
        return fail(ErrorCode::InvalidArgument, "config: log có trường lạ");
    }
    const Result<std::string> level = config::text(
        section, "level", "info", 8, "config: log.level phải là trace, debug, info, warn, error");
    if (!level) {
        return std::unexpected(level.error());
    }
    constexpr std::array<std::pair<std::string_view, core::LogLevel>, 5> kLevels{{
        {"trace", core::LogLevel::Trace},
        {"debug", core::LogLevel::Debug},
        {"info", core::LogLevel::Info},
        {"warn", core::LogLevel::Warn},
        {"error", core::LogLevel::Error},
    }};
    for (const auto& [name, value] : kLevels) {
        if (name == *level) {
            return value;
        }
    }
    return fail(ErrorCode::InvalidArgument,
                "config: log.level phải là trace, debug, info, warn, error");
}

// Các trường kích thước và số lượng của mục "http". Đọc hết rồi mới gán: sai một trường thì `out`
// không đổi.
[[nodiscard]] Result<void> read_http_sizes(const http::json::Object section,
                                           http::ServerConfig& out) {
    const std::array<Result<i64>, 8> reads{
        config::required_integer(section, "port", 1, 65'535,
                                 "config: http.port phải là số nguyên 1..65535"),
        config::integer(section, "io_threads", 1, 64, out.io_threads,
                        "config: http.io_threads phải là 1..64"),
        config::integer(section, "workers", 1, 256, out.workers,
                        "config: http.workers phải là 1..256"),
        config::integer(section, "queue_capacity", 1, 65'536, out.queue_capacity,
                        "config: http.queue_capacity phải là 1..65536"),
        config::integer(section, "max_connections", 1, 65'536, out.max_connections,
                        "config: http.max_connections phải là 1..65536"),
        config::integer(section, "max_header_bytes", 256, 65'535,
                        narrow<i64>(out.limits.max_header_bytes),
                        "config: http.max_header_bytes phải là 256..65535"),
        config::integer(section, "max_body_bytes", 0, i64{16} * 1024 * 1024,
                        narrow<i64>(out.limits.max_body_bytes),
                        "config: http.max_body_bytes phải là 0..16777216"),
        config::integer(section, "max_requests_per_connection", 1, 1'000'000,
                        out.limits.max_requests_per_connection,
                        "config: http.max_requests_per_connection phải là 1..1000000"),
    };
    for (const Result<i64>& read : reads) {
        if (!read) {
            return std::unexpected(read.error());
        }
    }
    out.port = narrow<u16>(*reads[0]);
    out.io_threads = narrow<u32>(*reads[1]);
    out.workers = narrow<u32>(*reads[2]);
    out.queue_capacity = narrow<u32>(*reads[3]);
    out.max_connections = narrow<u32>(*reads[4]);
    out.limits.max_header_bytes = narrow<usize>(*reads[5]);
    out.limits.max_body_bytes = narrow<usize>(*reads[6]);
    out.limits.max_requests_per_connection = narrow<u32>(*reads[7]);
    return {};
}

// Các trường thời gian của mục "http". Đọc hết rồi mới gán, như read_http_sizes.
[[nodiscard]] Result<void> read_http_timeouts(const http::json::Object section,
                                              http::ServerConfig& out) {
    http::Limits& limits = out.limits;
    const std::array<std::pair<core::Duration*, Result<core::Duration>>, 6> reads{{
        {&limits.read_timeout,
         config::milliseconds(section, "read_timeout_ms", 1, kMaxMilliseconds, limits.read_timeout,
                              "config: http.read_timeout_ms phải là 1..600000")},
        {&limits.idle_timeout,
         config::milliseconds(section, "idle_timeout_ms", 1, kMaxMilliseconds, limits.idle_timeout,
                              "config: http.idle_timeout_ms phải là 1..600000")},
        {&limits.write_timeout,
         config::milliseconds(section, "write_timeout_ms", 1, kMaxMilliseconds,
                              limits.write_timeout,
                              "config: http.write_timeout_ms phải là 1..600000")},
        {&limits.linger_timeout,
         config::milliseconds(section, "linger_timeout_ms", 1, kMaxMilliseconds,
                              limits.linger_timeout,
                              "config: http.linger_timeout_ms phải là 1..600000")},
        {&limits.handler_timeout,
         config::milliseconds(section, "handler_timeout_ms", 1, kMaxMilliseconds,
                              limits.handler_timeout,
                              "config: http.handler_timeout_ms phải là 1..600000")},
        {&out.check_interval,
         config::milliseconds(section, "check_interval_ms", 1, 10'000, out.check_interval,
                              "config: http.check_interval_ms phải là 1..10000")},
    }};
    for (const auto& [target, read] : reads) {
        if (!read) {
            return std::unexpected(read.error());
        }
    }
    for (const auto& [target, read] : reads) {
        *target = *read;
    }
    return {};
}

[[nodiscard]] Result<std::optional<HttpConfig>> read_http(const http::json::Object root) {
    const Result<std::optional<http::json::Object>> http =
        config::section(root, "http", "config: http phải là object");
    if (!http || !*http) {
        return http.transform([](const auto&) { return std::optional<HttpConfig>{}; });
    }
    const http::json::Object section = **http;
    constexpr std::array<std::string_view, 15> kFields{"address",
                                                       "port",
                                                       "io_threads",
                                                       "workers",
                                                       "queue_capacity",
                                                       "max_connections",
                                                       "max_header_bytes",
                                                       "max_body_bytes",
                                                       "read_timeout_ms",
                                                       "idle_timeout_ms",
                                                       "write_timeout_ms",
                                                       "linger_timeout_ms",
                                                       "handler_timeout_ms",
                                                       "max_requests_per_connection",
                                                       "check_interval_ms"};
    if (!section.expect_only(kFields)) {
        return fail(ErrorCode::InvalidArgument, "config: http có trường lạ");
    }
    HttpConfig out;
    const Result<std::string> address = config::text(section, "address", out.address, 64,
                                                     "config: http.address phải là IP dạng số");
    if (!address) {
        return std::unexpected(address.error());
    }
    out.address = *address;
    if (Result<void> sizes = read_http_sizes(section, out.server); !sizes) {
        return std::unexpected(sizes.error());
    }
    if (Result<void> timeouts = read_http_timeouts(section, out.server); !timeouts) {
        return std::unexpected(timeouts.error());
    }
    return std::optional<HttpConfig>{std::move(out)};
}

[[nodiscard]] Result<std::optional<DatabaseConfig>> read_database(const ConfigFile& file,
                                                                  const Environment environment) {
    const Result<std::optional<http::json::Object>> database =
        config::section(file.root(), "database", "config: database phải là object");
    if (!database || !*database) {
        return database.transform([](const auto&) { return std::optional<DatabaseConfig>{}; });
    }
    const http::json::Object section = **database;
    constexpr std::array<std::string_view, 4> kFields{
        "conninfo_file", "connect_timeout_ms", "statement_timeout_ms", "client_check_interval_ms"};
    if (!section.expect_only(kFields)) {
        return fail(ErrorCode::InvalidArgument, "config: database có trường lạ");
    }
    DatabaseConfig out;
    const Result<std::string> path = config::required_text(
        section, "conninfo_file", 1'024, "config: database.conninfo_file phải là đường dẫn");
    if (!path) {
        return std::unexpected(path.error());
    }
    Result<std::string> conninfo = detail::read_secret_file(file.resolve(*path), environment);
    if (!conninfo) {
        return std::unexpected(conninfo.error());
    }
    out.conninfo = std::move(*conninfo);
    const std::array<std::pair<core::Duration*, Result<core::Duration>>, 3> reads{{
        {&out.connect_timeout,
         config::milliseconds(section, "connect_timeout_ms", 1, kMaxMilliseconds,
                              out.connect_timeout,
                              "config: database.connect_timeout_ms phải là 1..600000")},
        {&out.options.statement_timeout,
         config::milliseconds(section, "statement_timeout_ms", 0, kMaxMilliseconds,
                              out.options.statement_timeout,
                              "config: database.statement_timeout_ms phải là 0..600000")},
        {&out.options.client_check_interval,
         config::milliseconds(section, "client_check_interval_ms", 0, kMaxMilliseconds,
                              out.options.client_check_interval,
                              "config: database.client_check_interval_ms phải là 0..600000")},
    }};
    for (const auto& [target, read] : reads) {
        if (!read) {
            return std::unexpected(read.error());
        }
        *target = *read;
    }
    return std::optional<DatabaseConfig>{std::move(out)};
}

}  // namespace

http::ServerConfig HttpConfig::server_config() const noexcept {
    http::ServerConfig config = server;
    config.address = address;
    return config;
}

// swap rồi ghi 0 bản cũ: chuỗi ngắn nằm ngay trong đối tượng (SSO), move chép nó mà không xoá.
DatabaseConfig::DatabaseConfig(DatabaseConfig&& other) noexcept
    : options(other.options), connect_timeout(other.connect_timeout) {
    conninfo.swap(other.conninfo);
    detail::wipe(other.conninfo);
}

DatabaseConfig& DatabaseConfig::operator=(DatabaseConfig&& other) noexcept {
    if (this != &other) {
        detail::wipe(conninfo);
        conninfo.swap(other.conninfo);
        detail::wipe(other.conninfo);
        options = other.options;
        connect_timeout = other.connect_timeout;
    }
    return *this;
}

DatabaseConfig::~DatabaseConfig() {
    detail::wipe(conninfo);
}

Result<CommonConfig> read_common(const ConfigFile& file,
                                 const std::span<const std::string_view> service_sections) {
    const http::json::Object root = file.root();
    for (const http::json::Member member : root) {
        if (std::ranges::find(kCommonSections, member.key) == kCommonSections.end() &&
            std::ranges::find(service_sections, member.key) == service_sections.end()) {
            return fail(ErrorCode::InvalidArgument, "config: trường lạ ở gốc của tệp cấu hình");
        }
    }
    CommonConfig out;
    const Result<Environment> environment = read_environment(root);
    if (!environment) {
        return std::unexpected(environment.error());
    }
    out.environment = *environment;
    const Result<core::LogLevel> level = read_log(root);
    if (!level) {
        return std::unexpected(level.error());
    }
    out.log_level = *level;
    Result<std::optional<HttpConfig>> http = read_http(root);
    if (!http) {
        return std::unexpected(http.error());
    }
    out.http = std::move(*http);
    Result<std::optional<DatabaseConfig>> database = read_database(file, out.environment);
    if (!database) {
        return std::unexpected(database.error());
    }
    out.database = std::move(*database);
    return out;
}

}  // namespace orion::service
