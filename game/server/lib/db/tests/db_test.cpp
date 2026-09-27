// server_db trên PostgreSQL thật (ARCH §8 mục 3). Test cần DB dùng fixture PostgresTest: không có
// ORION_TEST_POSTGRES thì bỏ qua, trừ khi ORION_REQUIRE_POSTGRES=1
// (tests/support/postgres_test.hpp). Test không cần DB (chuỗi kết nối sai, hạn đã qua, số tham số)
// chạy ở mọi nơi.

#include "game/server/lib/db/db.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/tests/support/postgres_test.hpp"
#include "game/server/lib/db/value.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::db {
namespace {

using testing::describe;

constexpr core::Duration kHour = core::Duration::seconds(3'600);

// Đồng hồ nhảy một giây mỗi lần được hỏi. Lần hỏi đầu của một lời gọi (kiểm hạn trước khi gửi) thấy
// còn hạn, lần hỏi sau (lúc chờ server) thấy đã quá: test "quá hạn giữa chừng" tất định mà không
// chờ thật (CLAUDE.md X.4).
class SteppingClock final : public core::MonotonicClock {
public:
    [[nodiscard]] core::MonoTime now() const noexcept override {
        return core::MonoTime::from_nanoseconds(calls_.fetch_add(1, std::memory_order_relaxed) *
                                                kStep);
    }
    // Mốc mà lần hỏi kế tiếp sẽ trả.
    [[nodiscard]] core::MonoTime upcoming() const noexcept {
        return core::MonoTime::from_nanoseconds(calls_.load(std::memory_order_relaxed) * kStep);
    }

private:
    static constexpr i64 kStep = 1'000'000'000;
    // relaxed: một luồng dùng; atomic chỉ vì now() là const.
    mutable std::atomic<i64> calls_{0};
};

class Database : public testing::PostgresTest {
protected:
    void create_items() {
        const Result<void> created = connection().execute_script(
            "CREATE TABLE item (id int8 PRIMARY KEY, count int8 NOT NULL)", deadline());
        ASSERT_TRUE(created.has_value()) << describe(created.error(), connection());
    }

    [[nodiscard]] std::optional<i64> item_count() {
        const Result<Rows> rows = connection().execute("SELECT count(*) FROM item", {}, deadline());
        if (!rows) {
            ADD_FAILURE() << describe(rows.error(), connection());
            return std::nullopt;
        }
        const Result<i64> count = rows->integer(0, 0);
        return count ? std::optional<i64>(*count) : std::nullopt;
    }
};

class DatabaseDeathTest : public testing::PostgresTest {};

TEST(Connection, MalformedConninfoIsInvalidArgument) {
    const core::FakeMonotonicClock clock;
    Connection connection(clock);
    const core::MonoTime deadline = clock.now() + kHour;
    const Result<void> missing_equals = connection.connect("host", deadline);
    ASSERT_FALSE(missing_equals.has_value());
    EXPECT_EQ(missing_equals.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(connection.last_error().empty());
    EXPECT_FALSE(connection.connected());
    EXPECT_EQ(connection.connect("postgresql://[::1", deadline).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(connection.connect(std::string_view("host=a\0b", 8), deadline).error().code(),
              ErrorCode::InvalidArgument);
    Connection negative(clock, Options{.statement_timeout = core::Duration::seconds(-1)});
    EXPECT_EQ(negative.connect("", deadline).error().code(), ErrorCode::InvalidArgument);
}

TEST(Connection, NothingIsSentAfterTheDeadline) {
    const core::FakeMonotonicClock clock;
    Connection connection(clock);
    const Result<void> connected = connection.connect("host=127.0.0.1", clock.now());
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error().code(), ErrorCode::DeadlineExceeded);
    const Result<Rows> rows = connection.execute("SELECT 1", {}, clock.now() + kHour);
    ASSERT_FALSE(rows.has_value());
    EXPECT_EQ(rows.error().code(), ErrorCode::Unavailable);
    EXPECT_EQ(connection.execute_script("SELECT 1", clock.now() + kHour).error().code(),
              ErrorCode::Unavailable);
}

TEST_F(Database, ParamsRoundTripThroughTheServer) {
    const std::array raw{std::byte{0}, std::byte{0xFF}, std::byte{0x80}, std::byte{0}};
    const core::WallTime when = core::WallTime::from_unix_microseconds(1'700'000'000'123'456);
    const core::WallTime before_1970 = core::WallTime::from_unix_microseconds(-86'400'000'001);
    const std::array params{
        Param::boolean(true),
        Param::integer(std::numeric_limits<i64>::min()),
        Param::real(-0.1),
        Param::text("Hà Nội ☃"),
        Param::bytes(raw),
        Param::time(when),
        Param::null(),
        Param::text(""),
        Param::time(before_1970),
        // Rỗng mà con trỏ null: libpq hiểu con trỏ null là NULL, nên server_db phải trỏ đi nơi
        // khác.
        Param::text(std::string_view{}),
        Param::bytes(std::span<const std::byte>{}),
    };
    const Result<Rows> rows = connection().execute(
        "SELECT $1::bool, $2::int8, $3::float8, $4::text, $5::bytea, $6::timestamptz, $7::int8, "
        "$8::text, $9::timestamptz, $10::text, $11::bytea",
        params, deadline());
    ASSERT_TRUE(rows.has_value()) << describe(rows.error(), connection());
    ASSERT_EQ(rows->size(), 1U);
    ASSERT_EQ(rows->columns(), 11U);
    EXPECT_EQ(rows->boolean(0, 0), true);
    EXPECT_EQ(rows->integer(0, 1), std::numeric_limits<i64>::min());
    EXPECT_EQ(rows->real(0, 2), -0.1);
    EXPECT_EQ(rows->text(0, 3), "Hà Nội ☃");
    const Result<std::span<const std::byte>> bytes = rows->bytes(0, 4);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_TRUE(std::ranges::equal(*bytes, raw));
    EXPECT_EQ(rows->time(0, 5), when);
    EXPECT_EQ(rows->is_null(0, 6), true);
    EXPECT_EQ(rows->integer(0, 6).error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(rows->is_null(0, 7), false);
    EXPECT_EQ(rows->text(0, 7), "");
    EXPECT_EQ(rows->time(0, 8), before_1970);
    EXPECT_EQ(rows->is_null(0, 9), false);
    EXPECT_EQ(rows->text(0, 9), "");
    EXPECT_EQ(rows->is_null(0, 10), false);
    const Result<std::span<const std::byte>> empty = rows->bytes(0, 10);
    EXPECT_TRUE(empty.has_value() && empty->empty());
}

TEST_F(Database, ReadsNarrowerColumnTypes) {
    const Result<Rows> rows = connection().execute(
        "SELECT (-7)::int2, 2147483647::int4, 1.5::float4, 'abc'::varchar(8), current_user", {},
        deadline());
    ASSERT_TRUE(rows.has_value()) << describe(rows.error(), connection());
    EXPECT_EQ(rows->integer(0, 0), -7);
    EXPECT_EQ(rows->integer(0, 1), std::numeric_limits<i32>::max());
    EXPECT_EQ(rows->real(0, 2), 1.5);
    EXPECT_EQ(rows->text(0, 3), "abc");
    // current_user có kiểu name.
    const Result<std::string_view> user = rows->text(0, 4);
    EXPECT_TRUE(user.has_value() && !user->empty());
}

TEST_F(Database, WrongTypesAndCellsOutsideTheTableAreErrors) {
    const Result<Rows> rows = connection().execute("SELECT 'x'::text, 1::int8", {}, deadline());
    ASSERT_TRUE(rows.has_value()) << describe(rows.error(), connection());
    const Result<i64> as_integer = rows->integer(0, 0);
    ASSERT_FALSE(as_integer.has_value());
    EXPECT_EQ(as_integer.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(as_integer.error().detail(), oid::kText);
    EXPECT_EQ(rows->text(0, 1).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(rows->integer(1, 0).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(rows->integer(0, 2).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(rows->is_null(5, 5).error().code(), ErrorCode::OutOfRange);
}

TEST_F(Database, CountsAffectedRows) {
    create_items();
    const Result<Rows> inserted = connection().execute(
        "INSERT INTO item SELECT n, 0 FROM generate_series(1, 5) AS n", {}, deadline());
    ASSERT_TRUE(inserted.has_value()) << describe(inserted.error(), connection());
    EXPECT_EQ(inserted->affected(), 5U);
    EXPECT_EQ(inserted->size(), 0U);
    const std::array limit{Param::integer(3)};
    const Result<Rows> updated =
        connection().execute("UPDATE item SET count = count + 1 WHERE id <= $1", limit, deadline());
    ASSERT_TRUE(updated.has_value()) << describe(updated.error(), connection());
    EXPECT_EQ(updated->affected(), 3U);
    const Result<Rows> selected =
        connection().execute("SELECT id FROM item ORDER BY id", {}, deadline());
    ASSERT_TRUE(selected.has_value()) << describe(selected.error(), connection());
    EXPECT_EQ(selected->size(), 5U);
    EXPECT_EQ(selected->affected(), 5U);
    EXPECT_EQ(selected->integer(4, 0), 5);
    const Result<Rows> none =
        connection().execute("DELETE FROM item WHERE id > 100", {}, deadline());
    ASSERT_TRUE(none.has_value()) << describe(none.error(), connection());
    EXPECT_EQ(none->affected(), 0U);
    const Result<Rows> index =
        connection().execute("CREATE INDEX item_count ON item (count)", {}, deadline());
    ASSERT_TRUE(index.has_value()) << describe(index.error(), connection());
    EXPECT_EQ(index->affected(), 0U);
}

TEST_F(Database, DuplicateKeyIsAlreadyExistsWithItsSqlstate) {
    create_items();
    const std::array key{Param::integer(1)};
    const Result<Rows> first =
        connection().execute("INSERT INTO item VALUES ($1, 0)", key, deadline());
    ASSERT_TRUE(first.has_value()) << describe(first.error(), connection());
    const Result<Rows> again =
        connection().execute("INSERT INTO item VALUES ($1, 0)", key, deadline());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(again.error().detail(), kUniqueViolation);
    EXPECT_FALSE(connection().last_error().empty());
    // Lỗi của một câu không làm hỏng kết nối; lời gọi thành công xoá thông điệp lỗi.
    EXPECT_TRUE(connection().connected());
    EXPECT_TRUE(connection().execute("SELECT 1", {}, deadline()).has_value());
    EXPECT_TRUE(connection().last_error().empty());
}

TEST_F(Database, ServerErrorsMapByTheirSqlstate) {
    const Result<Rows> syntax = connection().execute("SELEC 1", {}, deadline());
    ASSERT_FALSE(syntax.has_value());
    EXPECT_EQ(syntax.error().code(), ErrorCode::Internal);
    EXPECT_EQ(syntax.error().detail(), sqlstate_code("42601"));
    EXPECT_EQ(
        connection().execute("SELECT * FROM khong_co_bang_nay", {}, deadline()).error().detail(),
        sqlstate_code("42P01"));
    // Chuỗi UTF-8 hỏng: server kiểm mã hoá khi nhận text ở dạng nhị phân.
    const std::array broken{Param::text("\xC3\x28")};
    const Result<Rows> utf8 = connection().execute("SELECT $1::text", broken, deadline());
    ASSERT_FALSE(utf8.has_value());
    EXPECT_EQ(utf8.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(utf8.error().detail(), sqlstate_code("22021"));
    // int8 không vừa cột int4.
    const std::array big{Param::integer(i64{1} << 40U)};
    const Result<Rows> narrow = connection().execute("SELECT $1::int4", big, deadline());
    ASSERT_FALSE(narrow.has_value());
    EXPECT_EQ(narrow.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(narrow.error().detail(), sqlstate_code("22003"));
}

TEST_F(Database, AcceptsUpToMaxParams) {
    std::vector<Param> params;
    params.reserve(kMaxParams);
    for (usize i = 0; i < kMaxParams; ++i) {
        params.push_back(Param::integer(static_cast<i64>(i)));
    }
    const Result<Rows> rows = connection().execute(
        "SELECT $1::int8 + $2 + $3 + $4 + $5 + $6 + $7 + $8 + $9 + $10 + $11 + $12 + $13 + $14 + "
        "$15 + $16 + $17 + $18 + $19 + $20 + $21 + $22 + $23 + $24 + $25 + $26 + $27 + $28 + $29 + "
        "$30 + $31 + $32",
        params, deadline());
    ASSERT_TRUE(rows.has_value()) << describe(rows.error(), connection());
    EXPECT_EQ(rows->integer(0, 0), 496);  // 0 + 1 + ... + 31
}

TEST_F(Database, ScriptsRunAsOneImplicitTransaction) {
    // Câu thứ ba lỗi lúc chạy, sau khi hai câu đầu đã chạy: cả script bị huỷ, bảng không còn.
    const Result<void> broken = connection().execute_script(
        "CREATE TABLE temporary_item (x int8); INSERT INTO temporary_item VALUES (1); "
        "SELECT 1 / 0; INSERT INTO temporary_item VALUES (2)",
        deadline());
    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().detail(), sqlstate_code("22012"));
    const Result<Rows> gone =
        connection().execute("SELECT to_regclass('temporary_item') IS NULL", {}, deadline());
    ASSERT_TRUE(gone.has_value()) << describe(gone.error(), connection());
    EXPECT_EQ(gone->boolean(0, 0), true);
    // NOTICE và WARNING của server chỉ được log, không làm lời gọi lỗi.
    const Result<void> noisy = connection().execute_script(
        "DO $$ BEGIN RAISE NOTICE 'xin chào'; RAISE WARNING 'cảnh báo'; END $$", deadline());
    EXPECT_TRUE(noisy.has_value()) << describe(noisy.error(), connection());
    EXPECT_EQ(
        connection().execute_script(std::string_view("SELECT 1\0", 9), deadline()).error().code(),
        ErrorCode::InvalidArgument);
}

TEST_F(Database, TransactionsCommitAndRollBack) {
    create_items();
    {
        Result<Transaction> rolled_back = connection().begin(Isolation::ReadCommitted, deadline());
        ASSERT_TRUE(rolled_back.has_value()) << describe(rolled_back.error(), connection());
        ASSERT_TRUE(rolled_back->execute("INSERT INTO item VALUES (1, 0)").has_value());
        ASSERT_TRUE(rolled_back->rollback().has_value());
    }
    EXPECT_EQ(item_count(), 0);
    {
        Result<Transaction> committed = connection().begin(Isolation::RepeatableRead, deadline());
        ASSERT_TRUE(committed.has_value()) << describe(committed.error(), connection());
        ASSERT_TRUE(committed->execute("INSERT INTO item VALUES (2, 0)").has_value());
        const Result<void> done = committed->commit();
        ASSERT_TRUE(done.has_value()) << describe(done.error(), connection());
    }
    EXPECT_EQ(item_count(), 1);
    {
        // Bị huỷ khi chưa commit: ROLLBACK.
        Result<Transaction> abandoned = connection().begin(Isolation::Serializable, deadline());
        ASSERT_TRUE(abandoned.has_value()) << describe(abandoned.error(), connection());
        ASSERT_TRUE(abandoned->execute("INSERT INTO item VALUES (3, 0)").has_value());
        ASSERT_TRUE(abandoned->execute_script("INSERT INTO item VALUES (4, 0)").has_value());
    }
    EXPECT_EQ(item_count(), 1);
    EXPECT_TRUE(connection().connected());
}

TEST_F(Database, AFailedStatementAbortsTheWholeTransaction) {
    create_items();
    ASSERT_TRUE(connection().execute("INSERT INTO item VALUES (1, 0)", {}, deadline()).has_value());
    Result<Transaction> transaction = connection().begin(Isolation::ReadCommitted, deadline());
    ASSERT_TRUE(transaction.has_value()) << describe(transaction.error(), connection());
    ASSERT_TRUE(transaction->execute("INSERT INTO item VALUES (2, 0)").has_value());
    EXPECT_EQ(transaction->execute("INSERT INTO item VALUES (1, 0)").error().code(),
              ErrorCode::AlreadyExists);
    const Result<Rows> after = transaction->execute("SELECT 1");
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::Aborted);
    EXPECT_EQ(after.error().detail(), sqlstate_code("25P02"));
    // COMMIT của transaction đã hỏng thành ROLLBACK: phải là lỗi, không phải thành công.
    const Result<void> committed = transaction->commit();
    ASSERT_FALSE(committed.has_value());
    EXPECT_EQ(committed.error().code(), ErrorCode::Aborted);
    EXPECT_EQ(item_count(), 1);
}

// Write skew giữa hai transaction serializable: mỗi bên đọc dòng mà bên kia ghi. Bên commit trước
// qua; bên kia bị huỷ (40001) ở câu ghi hay ở COMMIT, tuỳ lúc PostgreSQL thấy vòng phụ thuộc.
TEST_F(Database, SerializationFailureIsAborted) {
    create_items();
    ASSERT_TRUE(
        connection().execute("INSERT INTO item VALUES (1, 0), (2, 0)", {}, deadline()).has_value());
    Result<Connection> other = connect_another(clock());
    ASSERT_TRUE(other.has_value()) << other.error().context();
    Result<Transaction> first = connection().begin(Isolation::Serializable, deadline());
    Result<Transaction> second = other->begin(Isolation::Serializable, deadline());
    ASSERT_TRUE(first.has_value() && second.has_value());
    ASSERT_TRUE(first->execute("SELECT count FROM item WHERE id = 2").has_value());
    ASSERT_TRUE(second->execute("SELECT count FROM item WHERE id = 1").has_value());
    ASSERT_TRUE(first->execute("UPDATE item SET count = 1 WHERE id = 1").has_value());
    const Result<Rows> write = second->execute("UPDATE item SET count = 1 WHERE id = 2");
    const Result<void> first_commit = first->commit();
    ASSERT_TRUE(first_commit.has_value()) << describe(first_commit.error(), connection());
    const Result<void> second_commit =
        write.has_value() ? second->commit() : Result<void>(std::unexpected(write.error()));
    ASSERT_FALSE(second_commit.has_value());
    EXPECT_EQ(second_commit.error().code(), ErrorCode::Aborted);
    EXPECT_EQ(second_commit.error().detail(), kSerializationFailure);
}

TEST_F(Database, LosingTheConnectionIsUnavailable) {
    Result<Connection> victim = connect_another(clock());
    ASSERT_TRUE(victim.has_value()) << victim.error().context();
    const Result<Rows> pid = victim->execute("SELECT pg_backend_pid()::int8", {}, deadline());
    ASSERT_TRUE(pid.has_value()) << describe(pid.error(), *victim);
    const Result<i64> number = pid->integer(0, 0);
    ASSERT_TRUE(number.has_value());
    // Tham số thứ hai (PostgreSQL 14) chờ tới khi backend thật sự thoát, nên câu sau của nạn nhân
    // luôn gặp một kết nối đã chết.
    const std::array target{Param::integer(*number)};
    const Result<Rows> killed =
        connection().execute("SELECT pg_terminate_backend($1::int4, 10000)", target, deadline());
    ASSERT_TRUE(killed.has_value()) << describe(killed.error(), connection());
    EXPECT_EQ(killed->boolean(0, 0), true);
    const Result<Rows> after = victim->execute("SELECT 1", {}, deadline());
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::Unavailable);
    EXPECT_FALSE(victim->connected());
    EXPECT_FALSE(victim->last_error().empty());
}

TEST_F(Database, UnreachableServerIsUnavailableWithAReason) {
    Connection nowhere(clock());
    // Cổng 1 của loopback không có ai nghe: kernel trả RST ngay.
    const Result<void> connected = nowhere.connect("host=127.0.0.1 port=1", deadline());
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error().code(), ErrorCode::Unavailable);
    EXPECT_FALSE(nowhere.last_error().empty());
    EXPECT_FALSE(nowhere.connected());
}

TEST_F(Database, DeadlineBeforeSendingKeepsTheConnection) {
    // Đồng hồ giả của fixture đứng yên: mốc hạn bằng lúc này là đã quá.
    const Result<Rows> late = connection().execute("SELECT 1", {}, clock().now());
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code(), ErrorCode::DeadlineExceeded);
    EXPECT_TRUE(connection().connected());
    EXPECT_EQ(connection().begin(Isolation::ReadCommitted, clock().now()).error().code(),
              ErrorCode::DeadlineExceeded);
    EXPECT_TRUE(connection().execute("SELECT 1", {}, deadline()).has_value());
}

TEST_F(Database, DeadlineWhileWaitingClosesTheConnection) {
    const SteppingClock stepping;
    Result<Connection> slow = connect_another(stepping);
    ASSERT_TRUE(slow.has_value()) << slow.error().context();
    // Lần hỏi đồng hồ kế tiếp (trước khi gửi) còn hạn; lần sau (lúc chờ kết quả) đã quá.
    const Result<Rows> sleeping = slow->execute(
        "SELECT pg_sleep(30)", {}, stepping.upcoming() + core::Duration::milliseconds(1));
    ASSERT_FALSE(sleeping.has_value());
    EXPECT_EQ(sleeping.error().code(), ErrorCode::DeadlineExceeded);
    EXPECT_FALSE(slow->connected());
    // Kết nối lại được, và kết nối mới dùng được.
    const Result<void> again = slow->connect(conninfo(), stepping.upcoming() + kHour);
    ASSERT_TRUE(again.has_value()) << describe(again.error(), *slow);
    EXPECT_TRUE(slow->execute("SELECT 1", {}, stepping.upcoming() + kHour).has_value());
}

TEST_F(Database, ConnectHonoursTheDeadline) {
    const SteppingClock stepping;
    Connection late(stepping);
    const Result<void> connected =
        late.connect(conninfo(), stepping.upcoming() + core::Duration::milliseconds(1));
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error().code(), ErrorCode::DeadlineExceeded);
    EXPECT_FALSE(late.connected());
}

TEST_F(Database, SessionUsesUtf8AndTheTimeoutsOfItsOptions) {
    Connection custom(clock(), Options{.statement_timeout = core::Duration::milliseconds(1'500),
                                       .client_check_interval = core::Duration::milliseconds(250),
                                       .application_name = "orion_db_test"});
    const Result<void> connected = custom.connect(conninfo(), deadline());
    ASSERT_TRUE(connected.has_value()) << describe(connected.error(), custom);
    const Result<Rows> settings = custom.execute(
        "SELECT current_setting('statement_timeout'), "
        "current_setting('client_connection_check_interval'), current_setting('client_encoding'), "
        "current_setting('application_name')",
        {}, deadline());
    ASSERT_TRUE(settings.has_value()) << describe(settings.error(), custom);
    EXPECT_EQ(settings->text(0, 0), "1500ms");
    EXPECT_EQ(settings->text(0, 1), "250ms");
    EXPECT_EQ(settings->text(0, 2), "UTF8");
    EXPECT_EQ(settings->text(0, 3), "orion_db_test");
}

// statement_timeout của server huỷ câu chạy quá (57014) mà không đóng kết nối.
TEST_F(Database, StatementTimeoutCancelsTheStatementOnly) {
    Connection strict(clock(), Options{.statement_timeout = core::Duration::milliseconds(20)});
    const Result<void> connected = strict.connect(conninfo(), deadline());
    ASSERT_TRUE(connected.has_value()) << describe(connected.error(), strict);
    const Result<Rows> sleeping = strict.execute("SELECT pg_sleep(10)", {}, deadline());
    ASSERT_FALSE(sleeping.has_value());
    EXPECT_EQ(sleeping.error().code(), ErrorCode::DeadlineExceeded);
    EXPECT_EQ(sleeping.error().detail(), sqlstate_code("57014"));
    EXPECT_TRUE(strict.connected());
    EXPECT_TRUE(strict.execute("SELECT 1", {}, deadline()).has_value());
}

TEST_F(Database, RowsOutliveTheirConnectionAndConnectionsMove) {
    Result<Connection> other = connect_another(clock());
    ASSERT_TRUE(other.has_value()) << other.error().context();
    Connection moved = std::move(*other);
    Result<Rows> rows = moved.execute("SELECT 42::int8", {}, deadline());
    ASSERT_TRUE(rows.has_value()) << describe(rows.error(), moved);
    moved.close();
    EXPECT_FALSE(moved.connected());
    const Rows kept = std::move(*rows);
    EXPECT_EQ(kept.integer(0, 0), 42);
    EXPECT_EQ(moved.execute("SELECT 1", {}, deadline()).error().code(), ErrorCode::Unavailable);
    // Gán chuyển: kết nối và kết quả cũ của bên nhận được giải phóng, bên nhận dùng tiếp được.
    Result<Connection> replacement = connect_another(clock());
    ASSERT_TRUE(replacement.has_value()) << replacement.error().context();
    moved = std::move(*replacement);
    Result<Rows> first = moved.execute("SELECT 1::int8", {}, deadline());
    Result<Rows> second = moved.execute("SELECT 2::int8", {}, deadline());
    ASSERT_TRUE(first.has_value() && second.has_value());
    *first = std::move(*second);
    EXPECT_EQ(first->integer(0, 0), 2);
}

// Câu COMMIT hay ROLLBACK không tới được server vì đã quá hạn: transaction còn mở ở server, nên kết
// nối bị đóng (server tự ROLLBACK) thay vì được dùng tiếp khi còn dở transaction.
TEST_F(Database, CommitOrRollbackPastTheDeadlineClosesTheConnection) {
    create_items();
    for (const bool commit : {true, false}) {
        core::FakeMonotonicClock clock;
        Result<Connection> other = connect_another(clock);
        ASSERT_TRUE(other.has_value()) << other.error().context();
        Result<Transaction> transaction =
            other->begin(Isolation::ReadCommitted, clock.now() + core::Duration::seconds(1));
        ASSERT_TRUE(transaction.has_value()) << describe(transaction.error(), *other);
        ASSERT_TRUE(transaction->execute("INSERT INTO item VALUES (7, 0)").has_value());
        clock.advance(core::Duration::seconds(2));
        const Result<void> finished = commit ? transaction->commit() : transaction->rollback();
        ASSERT_FALSE(finished.has_value()) << commit;
        EXPECT_EQ(finished.error().code(), ErrorCode::DeadlineExceeded) << commit;
        EXPECT_FALSE(other->connected()) << commit;
        // Dòng chưa bao giờ được commit.
        EXPECT_EQ(item_count(), 0) << commit;
    }
}

// Transaction sống lâu hơn phiên của nó (kết nối bị đóng, hay backend bị giết): huỷ transaction
// không làm gì hỏng, và kết nối kết nối lại được.
TEST_F(Database, TransactionsSurviveLosingTheirSession) {
    Result<Connection> other = connect_another(clock());
    ASSERT_TRUE(other.has_value()) << other.error().context();
    {
        Result<Transaction> transaction = other->begin(Isolation::ReadCommitted, deadline());
        ASSERT_TRUE(transaction.has_value()) << describe(transaction.error(), *other);
        other->close();
        EXPECT_EQ(transaction->execute("SELECT 1").error().code(), ErrorCode::Unavailable);
    }
    ASSERT_TRUE(other->connect(conninfo(), deadline()).has_value());
    const Result<Rows> pid = other->execute("SELECT pg_backend_pid()::int8", {}, deadline());
    ASSERT_TRUE(pid.has_value()) << describe(pid.error(), *other);
    const Result<i64> number = pid->integer(0, 0);
    ASSERT_TRUE(number.has_value());
    {
        Result<Transaction> transaction = other->begin(Isolation::ReadCommitted, deadline());
        ASSERT_TRUE(transaction.has_value()) << describe(transaction.error(), *other);
        const std::array target{Param::integer(*number)};
        const Result<Rows> killed = connection().execute(
            "SELECT pg_terminate_backend($1::int4, 10000)", target, deadline());
        ASSERT_TRUE(killed.has_value()) << describe(killed.error(), connection());
        // Bị huỷ ở đây: ROLLBACK gặp kết nối đã chết, nên kết nối bị đóng.
    }
    EXPECT_FALSE(other->connected());
    EXPECT_TRUE(other->connect(conninfo(), deadline()).has_value());
}

TEST_F(Database, CopyIsRefusedAndClosesTheConnection) {
    Result<Connection> other = connect_another(clock());
    ASSERT_TRUE(other.has_value()) << other.error().context();
    const Result<Rows> copied = other->execute("COPY (SELECT 1) TO STDOUT", {}, deadline());
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().code(), ErrorCode::Unimplemented);
    EXPECT_FALSE(other->connected());
}

// Tham số lớn hơn bộ đệm gửi của socket: PQflush phải chạy nhiều vòng, có chờ socket ghi được.
TEST_F(Database, LargeParamsAreSentInPieces) {
    const std::vector<std::byte> large(usize{16} << 20U, std::byte{0x5A});
    const std::array params{Param::bytes(large)};
    const Result<Rows> rows =
        connection().execute("SELECT length($1), get_byte($1, 16777215)", params, deadline());
    ASSERT_TRUE(rows.has_value()) << describe(rows.error(), connection());
    EXPECT_EQ(rows->integer(0, 0), i64{16} << 20U);
    EXPECT_EQ(rows->integer(0, 1), 0x5A);
}

#if !ORION_SHIP
TEST(ConnectionDeathTest, MoreThanMaxParamsIsAProgrammingError) {
    const core::FakeMonotonicClock clock;
    Connection connection(clock);
    const std::vector<Param> params(kMaxParams + 1, Param::null());
    EXPECT_DEATH(
        {
            const Result<Rows> rows = connection.execute("SELECT 1", params, clock.now() + kHour);
            EXPECT_FALSE(rows.has_value());
        },
        "tham số");
}

TEST_F(DatabaseDeathTest, DestroyingAConnectionWithAnOpenTransactionIsFatal) {
    EXPECT_DEATH(
        {
            std::optional<Connection> other;
            other.emplace(clock());
            const Result<void> connected = other->connect(conninfo(), deadline());
            const Result<Transaction> open =
                connected ? other->begin(Isolation::ReadCommitted, deadline())
                          : Result<Transaction>(std::unexpected(connected.error()));
            if (open.has_value()) {
                other.reset();
            }
        },
        "transaction mở");
}
#endif

}  // namespace
}  // namespace orion::db
