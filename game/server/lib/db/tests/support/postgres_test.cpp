#include "game/server/lib/db/tests/support/postgres_test.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/environment.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/value.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace orion::db::testing {
namespace {

// Đồng hồ giả đứng yên, nên một giờ sau lúc bắt đầu là "không bao giờ" với mọi test.
constexpr core::Duration kNever = core::Duration::seconds(3'600);

[[nodiscard]] Result<void> enter_schema(Connection& connection, const std::string& schema,
                                        const core::MonoTime deadline) {
    const std::array params{Param::text(schema)};
    const Result<Rows> set =
        connection.execute("SELECT set_config('search_path', $1, false)", params, deadline);
    if (!set) {
        return std::unexpected(set.error());
    }
    return {};
}

}  // namespace

std::string describe(const Error& error, const Connection& connection) {
    return std::format("{} — {}", error, connection.last_error());
}

Result<void> apply_repository_migrations(Connection& connection, const core::MonoTime deadline) {
    // u8: CMake ghi đường dẫn theo UTF-8, và path từ chuỗi hẹp trên Windows đọc theo code page
    // ANSI.
    const std::filesystem::path dir(u8"" ORION_MIGRATIONS_DIR);
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end;
         it.increment(error)) {
        if (it->path().extension() == ".sql") {
            files.push_back(it->path());
        }
    }
    if (error) {
        return fail(ErrorCode::Io, "test: không đọc được db/migrations/", error.value());
    }
    std::ranges::sort(files);
    for (const std::filesystem::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) {
            return fail(ErrorCode::Io, "test: không mở được tệp migration");
        }
        const std::string script{std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>()};
        if (const Result<void> applied = connection.execute_script(script, deadline); !applied) {
            return applied;
        }
    }
    return {};
}

void PostgresTest::SetUp() {
    std::optional<std::string> conninfo = core::environment_variable("ORION_TEST_POSTGRES");
    if (!conninfo) {
        if (core::environment_variable("ORION_REQUIRE_POSTGRES") == "1") {
            FAIL() << "ORION_REQUIRE_POSTGRES=1 mà ORION_TEST_POSTGRES chưa đặt";
        }
        GTEST_SKIP() << "ORION_TEST_POSTGRES chưa đặt: bỏ qua test cần PostgreSQL";
    }
    conninfo_ = std::move(*conninfo);
    Connection& db = connection_.emplace(clock_);
    const Result<void> connected = db.connect(conninfo_, deadline());
    ASSERT_TRUE(connected.has_value()) << describe(connected.error(), db);
    // pid của backend là duy nhất giữa các kết nối đang sống trên server.
    const Result<Rows> pid = db.execute("SELECT pg_backend_pid()::int8", {}, deadline());
    ASSERT_TRUE(pid.has_value()) << describe(pid.error(), db);
    const Result<i64> number = pid->integer(0, 0);
    ASSERT_TRUE(number.has_value()) << number.error().context();
    schema_ = std::format("orion_test_{}", *number);
    // Tên chỉ có chữ và số, do chính test dựng: ghép vào script được (execute_script, db.hpp).
    const Result<void> created = db.execute_script(
        std::format("DROP SCHEMA IF EXISTS {0} CASCADE; CREATE SCHEMA {0}", schema_), deadline());
    ASSERT_TRUE(created.has_value()) << describe(created.error(), db);
    const Result<void> entered = enter_schema(db, schema_, deadline());
    ASSERT_TRUE(entered.has_value()) << describe(entered.error(), db);
}

void PostgresTest::TearDown() {
    if (!connection_.has_value() || schema_.empty()) {
        return;
    }
    Connection& db = *connection_;
    // Test mất kết nối hay quá hạn để kết nối đóng: mở lại chỉ để dọn schema.
    if (!db.connected()) {
        const Result<void> reconnected = db.connect(conninfo_, deadline());
        ASSERT_TRUE(reconnected.has_value()) << describe(reconnected.error(), db);
    }
    const Result<void> dropped =
        db.execute_script(std::format("DROP SCHEMA IF EXISTS {} CASCADE", schema_), deadline());
    EXPECT_TRUE(dropped.has_value()) << describe(dropped.error(), db);
}

Connection& PostgresTest::connection() noexcept {
    ORION_VERIFY(connection_.has_value(), "PostgresTest: chưa kết nối");
    return *connection_;
}

core::MonoTime PostgresTest::deadline() const noexcept {
    return clock_.now() + kNever;
}

Result<Connection> PostgresTest::connect_another(const core::MonotonicClock& clock) const {
    Connection other(clock);
    const core::MonoTime until = clock.now() + kNever;
    if (const Result<void> connected = other.connect(conninfo_, until); !connected) {
        return std::unexpected(connected.error());
    }
    if (const Result<void> entered = enter_schema(other, schema_, until); !entered) {
        return std::unexpected(entered.error());
    }
    return other;
}

}  // namespace orion::db::testing
