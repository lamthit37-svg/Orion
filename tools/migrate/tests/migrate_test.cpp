// orion_migrate (docs/formats/migrations.md, CLAUDE.md X.14): đọc và kiểm tệp migration ở mọi nơi;
// áp lên PostgreSQL thật qua fixture của server_db (bỏ qua khi không có ORION_TEST_POSTGRES).

#include "tools/migrate/migrate.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/tests/support/postgres_test.hpp"
#include "game/server/lib/db/value.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace orion::migrate {
namespace {

using db::testing::describe;

// Thư mục tạm riêng của một test, trong thư mục build: mỗi test một tên nên các test chạy song song
// không đụng nhau.
class ScratchDir {
public:
    ScratchDir() {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::path(ORION_MIGRATE_SCRATCH_DIR) /
                std::format("{}.{}", info->test_suite_name(), info->name());
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        EXPECT_FALSE(error) << error.message();
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    void write(const std::string_view name, const std::string_view content) const {
        std::ofstream out(path_ / name, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        EXPECT_TRUE(out.good()) << name;
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] Migration migration_of(const std::string_view file, std::string sql) {
    Outcome<Migration> parsed = parse(file, std::move(sql));
    EXPECT_TRUE(parsed.has_value()) << file;
    return parsed ? std::move(*parsed) : Migration{};
}

class Parse : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(crypto::initialize().has_value()); }
};

TEST_F(Parse, AcceptsWellFormedFiles) {
    const std::string sql = "CREATE TABLE item (id int8 PRIMARY KEY);\n";
    const Outcome<Migration> parsed = parse("0001_create_items.sql", sql);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().error.context();
    EXPECT_EQ(parsed->version, 1U);
    EXPECT_EQ(parsed->name, "create_items");
    EXPECT_EQ(parsed->sql, sql);
    EXPECT_TRUE(
        crypto::equal_constant_time(parsed->checksum, crypto::hash(std::as_bytes(std::span(sql)))));
    EXPECT_EQ(parsed->file_name(), "0001_create_items.sql");
    const std::string longest(60, 'z');
    EXPECT_TRUE(parse(std::format("9999_{}.sql", longest), sql).has_value());
    EXPECT_TRUE(parse("0042_v2_add_index_9.sql", sql).has_value());
}

TEST_F(Parse, RejectsMalformedNames) {
    const std::string too_long = std::format("0001_{}.sql", std::string(61, 'a'));
    for (const std::string_view name :
         {std::string_view("1_a.sql"), std::string_view("0001.sql"), std::string_view("0001_.sql"),
          std::string_view("0001_A.sql"), std::string_view("0001_a-b.sql"),
          std::string_view("0001_a.SQL"), std::string_view("0000_a.sql"),
          std::string_view("00010_a.sql"), std::string_view("abcd_a.sql"),
          std::string_view("0001_a.sql.bak"), std::string_view(too_long)}) {
        const Outcome<Migration> parsed = parse(name, "SELECT 1");
        ASSERT_FALSE(parsed.has_value()) << name;
        EXPECT_EQ(parsed.error().error.code(), ErrorCode::InvalidArgument) << name;
        EXPECT_EQ(parsed.error().subject, name);
    }
}

TEST_F(Parse, RejectsEmptyNulAndNonUtf8Content) {
    for (const std::string_view content :
         {std::string_view(""), std::string_view("SELECT 1;\0", 10),
          std::string_view("SELECT '\xC3\x28'")}) {
        const Outcome<Migration> parsed = parse("0001_a.sql", std::string(content));
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().error.code(), ErrorCode::InvalidArgument);
    }
}

TEST_F(Parse, LoadsSortsAndIgnoresOtherFiles) {
    const ScratchDir dir;
    dir.write("0002_second.sql", "SELECT 2");
    dir.write("0001_first.sql", "SELECT 1");
    dir.write("README.md", "không phải migration");
    const Outcome<std::vector<Migration>> loaded = load(dir.path());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().error.context() << loaded.error().subject;
    ASSERT_EQ(loaded->size(), 2U);
    EXPECT_EQ((*loaded)[0].file_name(), "0001_first.sql");
    EXPECT_EQ((*loaded)[1].file_name(), "0002_second.sql");
    EXPECT_EQ((*loaded)[1].sql, "SELECT 2");
}

TEST_F(Parse, EmptyDirectoryHasNoMigrations) {
    const ScratchDir dir;
    const Outcome<std::vector<Migration>> loaded = load(dir.path());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->empty());
}

TEST_F(Parse, LoadRejectsGapsDuplicatesAndBadEntries) {
    {
        const ScratchDir dir;
        dir.write("0001_a.sql", "SELECT 1");
        dir.write("0003_c.sql", "SELECT 3");
        const Outcome<std::vector<Migration>> gap = load(dir.path());
        ASSERT_FALSE(gap.has_value());
        EXPECT_EQ(gap.error().error.code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(gap.error().subject, "0003_c.sql");
        EXPECT_EQ(gap.error().error.detail(), 2);
    }
    {
        const ScratchDir dir;
        dir.write("0001_a.sql", "SELECT 1");
        dir.write("0001_b.sql", "SELECT 1");
        const Outcome<std::vector<Migration>> duplicate = load(dir.path());
        ASSERT_FALSE(duplicate.has_value());
        EXPECT_EQ(duplicate.error().error.code(), ErrorCode::InvalidArgument);
    }
    {
        const ScratchDir dir;
        dir.write("0002_b.sql", "SELECT 2");
        const Outcome<std::vector<Migration>> late_start = load(dir.path());
        ASSERT_FALSE(late_start.has_value());
        EXPECT_EQ(late_start.error().error.detail(), 1);
    }
    {
        const ScratchDir dir;
        dir.write("1_a.sql", "SELECT 1");
        EXPECT_EQ(load(dir.path()).error().subject, "1_a.sql");
    }
    {
        const ScratchDir dir;
        std::error_code error;
        std::filesystem::create_directory(dir.path() / "0001_a.sql", error);
        ASSERT_FALSE(error);
        EXPECT_EQ(load(dir.path()).error().error.code(), ErrorCode::InvalidArgument);
    }
    {
        const ScratchDir dir;
        dir.write("0001_a.sql", std::string(kMaxMigrationBytes + 1, ' '));
        EXPECT_EQ(load(dir.path()).error().error.code(), ErrorCode::ResourceExhausted);
    }
    EXPECT_EQ(
        load(std::filesystem::path(ORION_MIGRATE_SCRATCH_DIR) / "khong_co").error().error.code(),
        ErrorCode::NotFound);
}

class Migrations : public db::testing::PostgresTest {
protected:
    void SetUp() override {
        ASSERT_TRUE(crypto::initialize().has_value());
        PostgresTest::SetUp();
    }

    [[nodiscard]] bool table_exists(const std::string_view name) {
        const std::array params{db::Param::text(name)};
        const Result<db::Rows> rows =
            connection().execute("SELECT to_regclass($1) IS NOT NULL", params, deadline());
        EXPECT_TRUE(rows.has_value()) << describe(rows.error(), connection());
        return rows && rows->boolean(0, 0) == true;
    }

    [[nodiscard]] std::vector<i64> history() {
        std::vector<i64> versions;
        const Result<db::Rows> rows = connection().execute(
            "SELECT version::int8 FROM schema_migrations ORDER BY version", {}, deadline());
        EXPECT_TRUE(rows.has_value()) << describe(rows.error(), connection());
        for (usize row = 0; rows && row < rows->size(); ++row) {
            versions.push_back(rows->integer(row, 0).value_or(-1));
        }
        return versions;
    }
};

TEST_F(Migrations, AppliesEachMigrationOnceAndRecordsIt) {
    const std::vector migrations{
        migration_of("0001_items.sql", "CREATE TABLE item (id int8 PRIMARY KEY);"),
        migration_of("0002_item_count.sql",
                     "ALTER TABLE item ADD COLUMN count int8 NOT NULL DEFAULT 0;"
                     "INSERT INTO item (id) VALUES (1);"),
    };
    std::vector<std::string> applied;
    const auto record = [&applied](const Migration& migration) {
        applied.push_back(migration.file_name());
    };
    const Outcome<Status> first = apply(connection(), migrations, clock(), {}, record);
    ASSERT_TRUE(first.has_value()) << first.error().error.context() << first.error().message;
    EXPECT_EQ(first->applied, 2U);
    EXPECT_EQ(first->pending, 0U);
    EXPECT_EQ(applied, (std::vector<std::string>{"0001_items.sql", "0002_item_count.sql"}));
    EXPECT_EQ(history(), (std::vector<i64>{1, 2}));
    // Chạy lại: không áp gì nữa.
    const Outcome<Status> again = apply(connection(), migrations, clock(), {}, record);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->applied, 2U);
    EXPECT_EQ(applied.size(), 2U);
    const Outcome<Status> checked = check(connection(), migrations, clock());
    ASSERT_TRUE(checked.has_value());
    EXPECT_EQ(checked->pending, 0U);
    const Result<db::Rows> stored = connection().execute(
        "SELECT name, checksum FROM schema_migrations WHERE version = 2", {}, deadline());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->text(0, 0), "item_count");
    const Result<std::span<const std::byte>> checksum = stored->bytes(0, 1);
    ASSERT_TRUE(checksum.has_value());
    EXPECT_TRUE(crypto::equal_constant_time(*checksum, migrations[1].checksum.view()));
}

TEST_F(Migrations, CheckReportsPendingWithoutWriting) {
    const std::vector migrations{migration_of("0001_items.sql", "CREATE TABLE item (id int8);")};
    const Outcome<Status> status = check(connection(), migrations, clock());
    ASSERT_TRUE(status.has_value()) << status.error().error.context();
    EXPECT_EQ(status->applied, 0U);
    EXPECT_EQ(status->pending, 1U);
    EXPECT_FALSE(table_exists("schema_migrations"));
    EXPECT_FALSE(table_exists("item"));
}

TEST_F(Migrations, EditedOrRenamedMigrationsAreRejected) {
    const std::vector original{migration_of("0001_items.sql", "CREATE TABLE item (id int8);")};
    ASSERT_TRUE(apply(connection(), original, clock()).has_value());
    for (const std::vector<Migration>& changed :
         {std::vector{migration_of("0001_items.sql", "CREATE TABLE item (id int4);")},
          std::vector{migration_of("0001_goods.sql", "CREATE TABLE item (id int8);")}}) {
        for (const bool applying : {false, true}) {
            const Outcome<Status> status = applying ? apply(connection(), changed, clock())
                                                    : check(connection(), changed, clock());
            ASSERT_FALSE(status.has_value());
            EXPECT_EQ(status.error().error.code(), ErrorCode::FailedPrecondition);
            EXPECT_EQ(status.error().subject, changed[0].file_name());
        }
    }
}

TEST_F(Migrations, DatabaseNewerThanCodeIsRejected) {
    const std::vector migrations{
        migration_of("0001_a.sql", "CREATE TABLE a (x int8);"),
        migration_of("0002_b.sql", "CREATE TABLE b (x int8);"),
    };
    ASSERT_TRUE(apply(connection(), migrations, clock()).has_value());
    const Outcome<Status> older = check(connection(), std::span(migrations).first(1), clock());
    ASSERT_FALSE(older.has_value());
    EXPECT_EQ(older.error().error.code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(older.error().subject, "0002_b.sql");
}

TEST_F(Migrations, HoleInTheHistoryIsRejected) {
    const std::vector migrations{
        migration_of("0001_a.sql", "CREATE TABLE a (x int8);"),
        migration_of("0002_b.sql", "CREATE TABLE b (x int8);"),
    };
    ASSERT_TRUE(apply(connection(), migrations, clock()).has_value());
    ASSERT_TRUE(connection()
                    .execute("DELETE FROM schema_migrations WHERE version = 1", {}, deadline())
                    .has_value());
    const Outcome<Status> status = check(connection(), migrations, clock());
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().error.code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(status.error().subject, "schema_migrations");
    EXPECT_EQ(status.error().error.detail(), 1);
}

// Bảng lịch sử là dữ liệu từ DB (CLAUDE.md X.5): dòng sai định dạng hay bảng cùng tên mà khác cột
// là lỗi có tên bảng, không phải assert.
TEST_F(Migrations, CorruptHistoryIsReported) {
    const std::vector migrations{migration_of("0001_a.sql", "CREATE TABLE a (x int8);")};
    ASSERT_TRUE(apply(connection(), migrations, clock()).has_value());
    ASSERT_TRUE(connection()
                    .execute("INSERT INTO schema_migrations (version, name, checksum) "
                             "VALUES (10000, 'z', decode(repeat('00', 32), 'hex'))",
                             {}, deadline())
                    .has_value());
    const Outcome<Status> out_of_range = check(connection(), migrations, clock());
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().error.code(), ErrorCode::DataLoss);
    EXPECT_EQ(out_of_range.error().subject, "schema_migrations");
    ASSERT_TRUE(connection()
                    .execute_script("DROP TABLE schema_migrations; "
                                    "CREATE TABLE schema_migrations (id int8)",
                                    deadline())
                    .has_value());
    const Outcome<Status> foreign = check(connection(), migrations, clock());
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error().error.code(), ErrorCode::Internal);
    EXPECT_EQ(foreign.error().subject, "schema_migrations");
    EXPECT_FALSE(foreign.error().message.empty());
}

// Migration lỗi giữa chừng: chính nó rollback, các migration sau không chạy, các migration trước
// giữ nguyên; sửa tệp rồi chạy lại thì áp tiếp.
TEST_F(Migrations, FailingMigrationRollsBackAndStopsTheRun) {
    std::vector migrations{
        migration_of("0001_a.sql", "CREATE TABLE a (x int8);"),
        migration_of("0002_b.sql", "CREATE TABLE b (x int8); SELECT 1 / 0;"),
        migration_of("0003_c.sql", "CREATE TABLE c (x int8);"),
    };
    const Outcome<Status> failed = apply(connection(), migrations, clock());
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().subject, "0002_b.sql");
    EXPECT_EQ(failed.error().error.detail(), db::sqlstate_code("22012"));
    EXPECT_FALSE(failed.error().message.empty());
    EXPECT_EQ(history(), std::vector<i64>{1});
    EXPECT_TRUE(table_exists("a"));
    EXPECT_FALSE(table_exists("b"));
    EXPECT_FALSE(table_exists("c"));
    migrations[1] = migration_of("0002_b.sql", "CREATE TABLE b (x int8);");
    const Outcome<Status> fixed = apply(connection(), migrations, clock());
    ASSERT_TRUE(fixed.has_value()) << fixed.error().error.context();
    EXPECT_EQ(history(), (std::vector<i64>{1, 2, 3}));
    EXPECT_TRUE(table_exists("c"));
}

TEST_F(Migrations, MigrationEndingItsOwnTransactionIsRejected) {
    for (const std::string_view end : {"COMMIT", "ROLLBACK"}) {
        const std::vector migrations{
            migration_of("0001_a.sql",
                         std::format("CREATE TABLE IF NOT EXISTS a (x int8); {};", end)),
        };
        const Outcome<Status> status = apply(connection(), migrations, clock());
        ASSERT_FALSE(status.has_value()) << end;
        EXPECT_EQ(status.error().error.code(), ErrorCode::FailedPrecondition) << end;
        EXPECT_EQ(status.error().subject, "0001_a.sql");
        EXPECT_TRUE(history().empty()) << end;
    }
}

// Nhánh "lần chạy khác vừa áp": apply_one gặp migration đã có trong lịch sử thì bỏ qua nếu khớp,
// báo lỗi nếu bị sửa.
TEST_F(Migrations, ApplyOneSkipsWhatIsAlreadyApplied) {
    const std::vector migrations{migration_of("0001_a.sql", "CREATE TABLE a (x int8);")};
    ASSERT_TRUE(apply(connection(), migrations, clock()).has_value());
    const Outcome<bool> again = apply_one(connection(), migrations[0], deadline());
    ASSERT_TRUE(again.has_value()) << again.error().error.context();
    EXPECT_FALSE(*again);
    const Migration edited = migration_of("0001_a.sql", "CREATE TABLE a (x int4);");
    const Outcome<bool> conflict = apply_one(connection(), edited, deadline());
    ASSERT_FALSE(conflict.has_value());
    EXPECT_EQ(conflict.error().error.code(), ErrorCode::FailedPrecondition);
}

TEST_F(Migrations, AnotherSessionContinuesTheSameHistory) {
    const std::vector migrations{
        migration_of("0001_a.sql", "CREATE TABLE a (x int8);"),
        migration_of("0002_b.sql", "CREATE TABLE b (x int8);"),
    };
    ASSERT_TRUE(apply(connection(), std::span(migrations).first(1), clock()).has_value());
    Result<db::Connection> other = connect_another(clock());
    ASSERT_TRUE(other.has_value()) << other.error().context();
    const Outcome<Status> status = apply(*other, migrations, clock());
    ASSERT_TRUE(status.has_value()) << status.error().error.context();
    EXPECT_EQ(history(), (std::vector<i64>{1, 2}));
}

TEST_F(Migrations, ZeroTimeoutFailsBeforeTouchingTheDatabase) {
    const std::vector migrations{migration_of("0001_a.sql", "CREATE TABLE a (x int8);")};
    const Outcome<Status> status =
        apply(connection(), migrations, clock(), Options{.timeout = core::Duration{}});
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().error.code(), ErrorCode::DeadlineExceeded);
    EXPECT_FALSE(table_exists("schema_migrations"));
    EXPECT_TRUE(connection().connected());
}

// CLAUDE.md X.14: mỗi migration của repo áp được lên schema rỗng, và lên schema của bản trước nó.
TEST_F(Migrations, RepositoryMigrationsApplyFromEmptyAndFromThePreviousVersion) {
    const Outcome<std::vector<Migration>> loaded = load(ORION_MIGRATIONS_DIR);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().error.context() << loaded.error().subject;
    const std::span<const Migration> all(*loaded);
    const Outcome<Status> from_empty = apply(connection(), all, clock());
    ASSERT_TRUE(from_empty.has_value())
        << from_empty.error().subject << ": " << from_empty.error().message;
    EXPECT_EQ(from_empty->applied, all.size());
    for (usize n = 1; n <= all.size(); ++n) {
        const std::string previous = std::format("{}_{}", schema(), n);
        ASSERT_TRUE(connection()
                        .execute_script(std::format("DROP SCHEMA IF EXISTS {0} CASCADE; "
                                                    "CREATE SCHEMA {0}; SET search_path TO {0}",
                                                    previous),
                                        deadline())
                        .has_value());
        ASSERT_TRUE(apply(connection(), all.first(n - 1), clock()).has_value()) << n;
        const Outcome<Status> upgraded = apply(connection(), all.first(n), clock());
        ASSERT_TRUE(upgraded.has_value())
            << all[n - 1].file_name() << ": " << upgraded.error().message;
        EXPECT_EQ(upgraded->applied, n);
        ASSERT_TRUE(connection()
                        .execute_script(std::format("SET search_path TO {}; DROP SCHEMA {} CASCADE",
                                                    schema(), previous),
                                        deadline())
                        .has_value());
    }
}

}  // namespace
}  // namespace orion::migrate
