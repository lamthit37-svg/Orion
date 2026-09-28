// Đọc trường cấu hình và phần chung của tệp cấu hình (docs/formats/service.md, mục "Cấu hình").

#include "engine/core/error.hpp"
#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "game/server/lib/http/json.hpp"
#include "game/server/lib/http/server.hpp"
#include "game/server/lib/service/service.hpp"
#include "game/server/lib/service/tests/support/files.hpp"

#include <gtest/gtest.h>

#include <array>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace orion::service {
namespace {

using io::testing::TempPath;

// Object gốc của `text`; `document` giữ cây.
[[nodiscard]] Result<http::json::Object> parse_object(http::json::Document& document,
                                                      const std::string_view text) {
    const Result<http::json::Value> root = document.parse(text);
    if (!root) {
        return std::unexpected(root.error());
    }
    return root->object();
}

// Mã lỗi của `result`; nullopt khi thành công.
template <class T>
[[nodiscard]] std::optional<ErrorCode> error_of(const Result<T>& result) {
    if (result) {
        return std::nullopt;
    }
    return result.error().code();
}

TEST(ConfigReaders, IntegerUsesTheFallbackOnlyWhenTheFieldIsAbsent) {
    http::json::Document document;
    const Result<http::json::Object> object =
        parse_object(document, R"({"n":7,"s":"7","big":11,"low":-1})");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(config::integer(*object, "missing", 0, 10, 3, "config: n phải là 0..10"), 3);
    EXPECT_EQ(config::integer(*object, "n", 0, 10, 3, "config: n phải là 0..10"), 7);
    for (const std::string_view key : {"s", "big", "low"}) {
        const Result<i64> read = config::integer(*object, key, 0, 10, 3, "config: n phải là 0..10");
        ASSERT_FALSE(read.has_value()) << key;
        EXPECT_EQ(read.error().code(), ErrorCode::InvalidArgument) << key;
        EXPECT_EQ(read.error().context(), "config: n phải là 0..10") << key;
    }
}

TEST(ConfigReaders, RequiredFieldsMustBePresent) {
    http::json::Document document;
    const Result<http::json::Object> object = parse_object(document, R"({"n":7,"t":"x"})");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(config::required_integer(*object, "n", 0, 10, "config: n"), 7);
    EXPECT_EQ(error_of(config::required_integer(*object, "m", 0, 10, "config: m")),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(config::required_text(*object, "t", 8, "config: t"), "x");
    EXPECT_EQ(error_of(config::required_text(*object, "u", 8, "config: u")),
              ErrorCode::InvalidArgument);
}

TEST(ConfigReaders, MillisecondsBecomeDurations) {
    http::json::Document document;
    const Result<http::json::Object> object = parse_object(document, R"({"t":250})");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(config::milliseconds(*object, "t", 1, 1'000, core::Duration::seconds(1), "config: t"),
              core::Duration::milliseconds(250));
    EXPECT_EQ(config::milliseconds(*object, "u", 1, 1'000, core::Duration::seconds(1), "config: u"),
              core::Duration::seconds(1));
    EXPECT_EQ(error_of(config::milliseconds(*object, "t", 1, 100, core::Duration::seconds(1),
                                            "config: t")),
              ErrorCode::InvalidArgument);
}

TEST(ConfigReaders, TextHasAByteLimit) {
    http::json::Document document;
    const Result<http::json::Object> object = parse_object(document, R"({"name":"orion","n":1})");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(config::text(*object, "name", "x", 5, "config: name"), "orion");
    EXPECT_EQ(config::text(*object, "missing", "x", 5, "config: missing"), "x");
    EXPECT_EQ(error_of(config::text(*object, "name", "x", 4, "config: name")),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(error_of(config::text(*object, "n", "x", 4, "config: n")),
              ErrorCode::InvalidArgument);
}

TEST(ConfigReaders, SectionIsAnOptionalObject) {
    http::json::Document document;
    const Result<http::json::Object> object =
        parse_object(document, R"({"http":{"port":80},"log":"info"})");
    ASSERT_TRUE(object.has_value());
    const Result<std::optional<http::json::Object>> http =
        config::section(*object, "http", "config: http");
    ASSERT_TRUE(http.has_value());
    const std::optional<http::json::Object>& found = *http;
    if (!found) {
        FAIL() << "thiếu mục http";
    }
    EXPECT_EQ(found->size(), 1U);
    const Result<std::optional<http::json::Object>> missing =
        config::section(*object, "database", "config: database");
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->has_value());
    EXPECT_EQ(error_of(config::section(*object, "log", "config: log")), ErrorCode::InvalidArgument);
}

// Tệp cấu hình trong thư mục tạm của test, và tệp chuỗi kết nối DB cạnh nó.
class CommonConfigTest : public ::testing::Test {
protected:
    // Ghi `text` vào tệp cấu hình rồi đọc phần chung, cho phép mục riêng "auth".
    [[nodiscard]] Result<CommonConfig> read(const std::string_view text) {
        testing::write_file(config_.path(), text);
        const Result<ConfigFile> file = ConfigFile::load(config_.path());
        if (!file) {
            return std::unexpected(file.error());
        }
        constexpr std::array<std::string_view, 1> kSections{"auth"};
        return read_common(*file, kSections);
    }

    // Mục "database" trỏ tới `conninfo` theo đường dẫn tương đối.
    [[nodiscard]] static std::string database_section(const TempPath& conninfo,
                                                      const std::string_view extra = "") {
        return std::format(R"("database":{{"conninfo_file":"{}"{}}})",
                           testing::file_name(conninfo.path()), extra);
    }

    [[nodiscard]] const TempPath& conninfo() const noexcept { return conninfo_; }
    [[nodiscard]] const TempPath& dev_conninfo() const noexcept { return dev_conninfo_; }

private:
    TempPath config_{"config.json"};
    TempPath conninfo_{"conninfo.txt"};
    TempPath dev_conninfo_{"conninfo.dev.txt"};
};

TEST_F(CommonConfigTest, MinimalFileTakesTheDefaults) {
    const Result<CommonConfig> common = read(R"({"environment":"production"})");
    ASSERT_TRUE(common.has_value());
    EXPECT_EQ(common->environment, Environment::Production);
    EXPECT_EQ(common->log_level, core::LogLevel::Info);
    EXPECT_FALSE(common->http.has_value());
    EXPECT_FALSE(common->database.has_value());
}

TEST_F(CommonConfigTest, EnvironmentIsRequiredAndClosed) {
    for (const std::string_view text : {R"({})", R"({"environment":"staging"})",
                                        R"({"environment":1})", R"({"environment":"Local"})"}) {
        EXPECT_EQ(error_of(read(text)), ErrorCode::InvalidArgument) << text;
    }
    const Result<CommonConfig> local = read(R"({"environment":"local"})");
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(local->environment, Environment::Local);
}

TEST_F(CommonConfigTest, UnknownFieldsAreRejectedButServiceSectionsPass) {
    EXPECT_EQ(error_of(read(R"({"environment":"local","enviroment":"local"})")),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(error_of(read(R"({"environment":"local","log":{"lvl":"info"}})")),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(error_of(read(R"({"environment":"local","http":{"port":80,"prot":1}})")),
              ErrorCode::InvalidArgument);
    EXPECT_TRUE(read(R"({"environment":"local","auth":{"anything":[1,2]}})").has_value());
}

TEST_F(CommonConfigTest, ReadsEveryLogLevel) {
    constexpr std::array<std::pair<std::string_view, core::LogLevel>, 5> kLevels{{
        {"trace", core::LogLevel::Trace},
        {"debug", core::LogLevel::Debug},
        {"info", core::LogLevel::Info},
        {"warn", core::LogLevel::Warn},
        {"error", core::LogLevel::Error},
    }};
    for (const auto& [name, level] : kLevels) {
        const Result<CommonConfig> common =
            read(std::format(R"({{"environment":"local","log":{{"level":"{}"}}}})", name));
        ASSERT_TRUE(common.has_value()) << name;
        EXPECT_EQ(common->log_level, level) << name;
    }
    EXPECT_EQ(error_of(read(R"({"environment":"local","log":{"level":"fatal"}})")),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(error_of(read(R"({"environment":"local","log":"info"})")),
              ErrorCode::InvalidArgument);
}

TEST_F(CommonConfigTest, HttpSectionMapsOntoTheServerConfig) {
    const Result<CommonConfig> common = read(R"({"environment":"local","http":{
        "address":"0.0.0.0","port":8080,"io_threads":2,"workers":8,"queue_capacity":128,
        "max_connections":512,"max_header_bytes":4096,"max_body_bytes":1024,
        "read_timeout_ms":1500,"idle_timeout_ms":30000,"write_timeout_ms":2500,
        "linger_timeout_ms":500,"handler_timeout_ms":3000,"max_requests_per_connection":10,
        "check_interval_ms":50}})");
    ASSERT_TRUE(common.has_value());
    const std::optional<HttpConfig>& section = common->http;
    if (!section) {
        FAIL() << "thiếu mục http";
    }
    const http::ServerConfig server = section->server_config();
    EXPECT_EQ(server.address, "0.0.0.0");
    EXPECT_EQ(server.port, 8080);
    EXPECT_EQ(server.io_threads, 2U);
    EXPECT_EQ(server.workers, 8U);
    EXPECT_EQ(server.queue_capacity, 128U);
    EXPECT_EQ(server.max_connections, 512U);
    EXPECT_EQ(server.limits.max_header_bytes, 4096U);
    EXPECT_EQ(server.limits.max_body_bytes, 1024U);
    EXPECT_EQ(server.limits.read_timeout, core::Duration::milliseconds(1500));
    EXPECT_EQ(server.limits.idle_timeout, core::Duration::seconds(30));
    EXPECT_EQ(server.limits.write_timeout, core::Duration::milliseconds(2500));
    EXPECT_EQ(server.limits.linger_timeout, core::Duration::milliseconds(500));
    EXPECT_EQ(server.limits.handler_timeout, core::Duration::seconds(3));
    EXPECT_EQ(server.limits.max_requests_per_connection, 10U);
    EXPECT_EQ(server.check_interval, core::Duration::milliseconds(50));
}

TEST_F(CommonConfigTest, HttpSectionNeedsOnlyThePort) {
    const Result<CommonConfig> common = read(R"({"environment":"local","http":{"port":8080}})");
    ASSERT_TRUE(common.has_value());
    const std::optional<HttpConfig>& section = common->http;
    if (!section) {
        FAIL() << "thiếu mục http";
    }
    const http::ServerConfig server = section->server_config();
    const http::ServerConfig defaults;
    EXPECT_EQ(server.address, "127.0.0.1");
    EXPECT_EQ(server.port, 8080);
    EXPECT_EQ(server.workers, defaults.workers);
    EXPECT_EQ(server.limits.max_body_bytes, defaults.limits.max_body_bytes);
    EXPECT_EQ(server.limits.idle_timeout, defaults.limits.idle_timeout);
    EXPECT_EQ(server.check_interval, defaults.check_interval);

    for (const std::string_view http :
         {R"({})", R"({"port":0})", R"({"port":65536})", R"({"port":"80"})",
          R"({"port":80,"workers":0})", R"({"port":80,"max_header_bytes":255})",
          R"({"port":80,"max_body_bytes":16777217})", R"({"port":80,"read_timeout_ms":0})",
          R"({"port":80,"check_interval_ms":10001})", R"({"port":80,"address":7})"}) {
        EXPECT_EQ(error_of(read(std::format(R"({{"environment":"local","http":{}}})", http))),
                  ErrorCode::InvalidArgument)
            << http;
    }
}

TEST_F(CommonConfigTest, DatabaseSectionReadsTheConninfoFileNextToTheConfig) {
    testing::write_file(conninfo().path(), "host=db dbname=orion\n");
    const Result<CommonConfig> common = read(std::format(
        R"({{"environment":"production",{}}})",
        database_section(conninfo(), R"(,"connect_timeout_ms":2000,"statement_timeout_ms":0,)"
                                     R"("client_check_interval_ms":500)")));
    ASSERT_TRUE(common.has_value());
    const std::optional<DatabaseConfig>& database = common->database;
    if (!database) {
        FAIL() << "thiếu mục database";
    }
    EXPECT_EQ(database->conninfo, "host=db dbname=orion");
    EXPECT_EQ(database->connect_timeout, core::Duration::seconds(2));
    EXPECT_EQ(database->options.statement_timeout, core::Duration{});
    EXPECT_EQ(database->options.client_check_interval, core::Duration::milliseconds(500));
}

TEST_F(CommonConfigTest, DatabaseSectionRejectsBadInput) {
    testing::write_file(conninfo().path(), "\r\n");
    EXPECT_EQ(error_of(read(
                  std::format(R"({{"environment":"local",{}}})", database_section(conninfo())))),
              ErrorCode::InvalidArgument);
    testing::write_file(conninfo().path(), "host=db");
    EXPECT_EQ(error_of(read(R"({"environment":"local","database":{}})")),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(error_of(read(std::format(R"({{"environment":"local",{}}})",
                                        database_section(conninfo(), R"(,"pool":4)")))),
              ErrorCode::InvalidArgument);
    const TempPath missing("missing.txt");
    EXPECT_EQ(
        error_of(read(std::format(R"({{"environment":"local",{}}})", database_section(missing)))),
        ErrorCode::NotFound);
}

TEST_F(CommonConfigTest, DevConninfoIsOnlyForLocal) {
    testing::write_file(dev_conninfo().path(), "host=localhost");
    EXPECT_EQ(error_of(read(std::format(R"({{"environment":"production",{}}})",
                                        database_section(dev_conninfo())))),
              ErrorCode::FailedPrecondition);
    const Result<CommonConfig> local =
        read(std::format(R"({{"environment":"local",{}}})", database_section(dev_conninfo())));
    // Bản ship từ chối bí mật dev ở mọi môi trường (X.9).
    ASSERT_EQ(local.has_value(), ORION_SHIP == 0);
    if (local) {
        const std::optional<DatabaseConfig>& database = local->database;
        if (!database) {
            FAIL() << "thiếu mục database";
        }
        EXPECT_EQ(database->conninfo, "host=localhost");
    }
}

TEST(DatabaseConfig, MoveTakesTheConninfoAndEmptiesTheSource) {
    DatabaseConfig source;
    source.conninfo = "host=db password=secret-that-is-longer-than-sso";
    source.connect_timeout = core::Duration::seconds(9);
    DatabaseConfig moved(std::move(source));
    EXPECT_EQ(moved.conninfo, "host=db password=secret-that-is-longer-than-sso");
    EXPECT_EQ(moved.connect_timeout, core::Duration::seconds(9));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): kiểm sau move.
    EXPECT_TRUE(source.conninfo.empty());

    DatabaseConfig assigned;
    assigned.conninfo = "short";
    assigned = std::move(moved);
    EXPECT_EQ(assigned.conninfo, "host=db password=secret-that-is-longer-than-sso");
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): kiểm sau move.
    EXPECT_TRUE(moved.conninfo.empty());
}

}  // namespace
}  // namespace orion::service
