// Test tiêm lỗi của ledger (CLAUDE.md X.9: crash hay retry ở bất kỳ điểm nào cũng không được nhân
// bản vật phẩm hay tiền; ledger.md, mục "Test").
//
// Trigger PL/pgSQL chỉ có trong schema của test đếm từng dòng mà một bút toán ghi vào các bảng
// ledger, và làm hỏng đúng dòng thứ k: báo lỗi (transaction rollback), hay tự ngắt kết nối của
// chính phiên đó, như khi tiến trình gọi crash giữa chừng. Một trigger hoãn tới commit làm hỏng
// chính lúc COMMIT. Sau mỗi lần hỏng, sổ phải y như trước và soát lành; retry với cùng khoá áp đúng
// một lần; retry thêm nữa trả replayed.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/value.hpp"
#include "game/server/lib/ledger/ledger.hpp"
#include "game/server/lib/ledger/tests/support/ledger_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace orion::ledger {
namespace {

using testing::key;
using testing::LedgerTest;

constexpr AssetId kGold{1};
constexpr AssetId kGem{2};
constexpr AssetId kSword{100};
constexpr AssetId kShield{101};
constexpr ItemId kNewShield{1'001};
constexpr ItemId kOldSword{2'002};

// Số dòng bút toán trao đổi dưới đây ghi: 1 bút toán, 3 số dư (trừ vàng của A, cộng đá của A, cộng
// vàng của B), 2 vật phẩm (tạo khiên, đổi chủ kiếm), 2 lượt chuyển, 2 dòng chuyển.
constexpr i64 kWritesPerTrade = 10;

constexpr std::string_view kFaultScript = R"sql(
CREATE TABLE fault_config (fire_at int8 NOT NULL, crash bool NOT NULL, at_commit bool NOT NULL);
INSERT INTO fault_config VALUES (0, false, false);
CREATE SEQUENCE fault_counter;

CREATE FUNCTION inject_fault() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    config fault_config;
BEGIN
    SELECT * INTO config FROM fault_config;
    IF config.fire_at > 0 AND nextval('fault_counter') = config.fire_at THEN
        IF config.crash THEN
            PERFORM pg_terminate_backend(pg_backend_pid());
        END IF;
        RAISE EXCEPTION 'lỗi tiêm ở lần ghi thứ % (%)', config.fire_at, TG_TABLE_NAME;
    END IF;
    RETURN NULL;
END
$$;

CREATE FUNCTION inject_commit_fault() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    config fault_config;
BEGIN
    SELECT * INTO config FROM fault_config;
    IF config.at_commit THEN
        IF config.crash THEN
            PERFORM pg_terminate_backend(pg_backend_pid());
        END IF;
        RAISE EXCEPTION 'lỗi tiêm lúc commit';
    END IF;
    RETURN NULL;
END
$$;

CREATE TRIGGER inject_fault AFTER INSERT OR UPDATE ON ledger_entries
    FOR EACH ROW EXECUTE FUNCTION inject_fault();
CREATE TRIGGER inject_fault AFTER INSERT OR UPDATE ON ledger_transfers
    FOR EACH ROW EXECUTE FUNCTION inject_fault();
CREATE TRIGGER inject_fault AFTER INSERT OR UPDATE ON ledger_balances
    FOR EACH ROW EXECUTE FUNCTION inject_fault();
CREATE TRIGGER inject_fault AFTER INSERT OR UPDATE ON ledger_items
    FOR EACH ROW EXECUTE FUNCTION inject_fault();
CREATE TRIGGER inject_fault AFTER INSERT OR UPDATE ON ledger_item_moves
    FOR EACH ROW EXECUTE FUNCTION inject_fault();
CREATE CONSTRAINT TRIGGER inject_commit_fault AFTER INSERT ON ledger_entries
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION inject_commit_fault();
)sql";

// Mọi thứ bút toán trao đổi chạm tới.
struct State {
    i64 a_gold = 0;
    i64 a_gems = 0;
    i64 b_gold = 0;
    i64 shield_owner = 0;  // 0 khi chưa có
    i64 shield_version = 0;
    i64 sword_owner = 0;
    i64 sword_version = 0;
    i64 entries = 0;
    i64 transfers = 0;
    i64 moves = 0;

    friend bool operator==(const State&, const State&) = default;
};

// gtest in State bằng toán tử này khi EXPECT_EQ đỏ.
std::ostream& operator<<(std::ostream& out, const State& s) {
    return out << std::format(
               "vàng A {}, đá A {}, vàng B {}, khiên {}@{}, kiếm {}@{}, nhật ký {}/{}/{}", s.a_gold,
               s.a_gems, s.b_gold, s.shield_owner, s.shield_version, s.sword_owner, s.sword_version,
               s.entries, s.transfers, s.moves);
}

class LedgerFaults : public LedgerTest {
protected:
    void SetUp() override {
        LedgerTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        ASSERT_TRUE(connection().execute_script(kFaultScript, deadline()).has_value())
            << connection().last_error();
        const Result<AccountId> loot = account_named(AccountKind::External, "loot");
        const Result<AccountId> a = new_holder();
        const Result<AccountId> b = new_holder();
        ASSERT_TRUE(loot && a && b);
        loot_ = *loot;
        a_ = *a;
        b_ = *b;
        const std::array gold{Transfer{loot_, a_, kGold, 100}, Transfer{loot_, b_, kGold, 50}};
        const std::array sword{ItemMove{kOldSword, kSword, loot_, b_}};
        const Result<Posted> seeded = post_committed(
            {.key = key("seed"), .reason = Reason::Grant, .transfers = gold, .items = sword});
        ASSERT_TRUE(seeded.has_value());
        transfers_ = {Transfer{a_, b_, kGold, 40}, Transfer{loot_, a_, kGem, 5}};
        items_ = {ItemMove{kNewShield, kShield, loot_, a_}, ItemMove{kOldSword, kSword, b_, a_}};
    }

    // A trả B 40 vàng, nhận 5 đá rơi ra, một khiên mới và thanh kiếm của B.
    [[nodiscard]] Entry trade() const {
        return {
            .key = key("trade"), .reason = Reason::Trade, .transfers = transfers_, .items = items_};
    }

    // Hỏng ở lần ghi thứ `fire_at` (0: không hỏng), hay lúc commit; crash hay chỉ báo lỗi.
    void arm(const i64 fire_at, const bool crash, const bool at_commit) {
        const std::string script = std::format(
            "UPDATE fault_config SET fire_at = {}, crash = {}, at_commit = {}; "
            "ALTER SEQUENCE fault_counter RESTART",
            fire_at, crash, at_commit);
        ASSERT_TRUE(connection().execute_script(script, deadline()).has_value())
            << connection().last_error();
    }

    [[nodiscard]] State snapshot() {
        State s;
        const auto read = [](const Result<i64>& value) {
            EXPECT_TRUE(value.has_value());
            return value ? *value : -1;
        };
        s.a_gold = read(balance_of(a_, kGold));
        s.a_gems = read(balance_of(a_, kGem));
        s.b_gold = read(balance_of(b_, kGold));
        const auto own = [](const Result<std::optional<ItemState>>& state, i64& owner,
                            i64& version) {
            EXPECT_TRUE(state.has_value());
            if (state && *state) {
                owner = (*state)->owner.value;
                version = (*state)->version;
            }
        };
        own(item_of(kNewShield), s.shield_owner, s.shield_version);
        own(item_of(kOldSword), s.sword_owner, s.sword_version);
        s.entries = read(count("SELECT count(*) FROM ledger_entries"));
        s.transfers = read(count("SELECT count(*) FROM ledger_transfers"));
        s.moves = read(count("SELECT count(*) FROM ledger_item_moves"));
        return s;
    }

    // Trạng thái sau khi bút toán trao đổi áp đúng một lần lên `before`.
    [[nodiscard]] State traded(State before) const {
        before.a_gold -= 40;
        before.b_gold += 40;
        before.a_gems += 5;
        before.shield_owner = a_.value;
        before.shield_version = 1;
        before.sword_owner = a_.value;
        before.sword_version += 1;
        before.entries += 1;
        before.transfers += 2;
        before.moves += 2;
        return before;
    }

    void expect_clean(const std::string_view when) {
        const Result<Audit> report = audit_now();
        ASSERT_TRUE(report.has_value()) << when;
        EXPECT_TRUE(report->clean()) << when << ": " << report->balance_mismatches << ' '
                                     << report->owner_mismatches << ' ' << report->broken_chains;
    }

    // Bút toán đã áp một lần: thêm ba lần retry nữa đều trả replayed và không đổi gì.
    void expect_applied_once(const State& before) {
        const State after = snapshot();
        EXPECT_EQ(after, traded(before));
        for (int retry = 0; retry < 3; ++retry) {
            const Result<Posted> again = post_committed(trade());
            ASSERT_TRUE(again.has_value());
            EXPECT_TRUE(again->replayed);
        }
        EXPECT_EQ(snapshot(), after);
        expect_clean("sau retry");
    }

    // Hỏng lần lượt ở từng lần ghi, mỗi lần trên một phiên mới, cho tới khi bút toán qua được vì
    // không còn lần ghi nào để hỏng: lần đó chính là retry áp bút toán.
    void fail_every_write(const bool crash) {
        const State before = snapshot();
        i64 failures = 0;
        for (i64 fire_at = 1; fire_at <= 2 * kWritesPerTrade; ++fire_at) {
            arm(fire_at, crash, false);
            Result<db::Connection> session = connect_another(clock());
            ASSERT_TRUE(session.has_value());
            const Result<Posted> attempt = post_committed(*session, trade());
            if (attempt) {
                EXPECT_FALSE(attempt->replayed);
                break;
            }
            ++failures;
            EXPECT_EQ(attempt.error().code(),
                      crash ? ErrorCode::Unavailable : ErrorCode::FailedPrecondition)
                << "lần ghi " << fire_at << ": " << session->last_error();
            EXPECT_EQ(session->connected(), !crash) << "lần ghi " << fire_at;
            EXPECT_EQ(snapshot(), before) << "lần ghi " << fire_at;
            expect_clean(std::format("sau lỗi ở lần ghi {}", fire_at));
        }
        EXPECT_EQ(failures, kWritesPerTrade);
        arm(0, false, false);
        expect_applied_once(before);
    }

private:
    AccountId loot_;
    AccountId a_;
    AccountId b_;
    std::array<Transfer, 2> transfers_{};
    std::array<ItemMove, 2> items_{};
};

TEST_F(LedgerFaults, AnErrorAtAnyWriteRollsBackAndTheRetryAppliesOnce) {
    fail_every_write(false);
}

TEST_F(LedgerFaults, ACrashAtAnyWriteRollsBackAndTheRetryAppliesOnce) {
    fail_every_write(true);
}

TEST_F(LedgerFaults, AnErrorAtCommitRollsBackAndTheRetryAppliesOnce) {
    const State before = snapshot();
    arm(0, false, true);
    const Result<Posted> attempt = post_committed(trade());
    ASSERT_FALSE(attempt.has_value());
    EXPECT_EQ(attempt.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(snapshot(), before);
    expect_clean("sau lỗi lúc commit");
    arm(0, false, false);
    const Result<Posted> retried = post_committed(trade());
    ASSERT_TRUE(retried.has_value());
    EXPECT_FALSE(retried->replayed);
    expect_applied_once(before);
}

// Kết nối mất giữa COMMIT: bên gọi không biết đã commit chưa, nên retry. Trigger hoãn chạy trước
// khi commit được ghi, nên ở đây nó chưa commit và retry áp.
TEST_F(LedgerFaults, ACrashDuringCommitIsResolvedByRetry) {
    const State before = snapshot();
    arm(0, true, true);
    Result<db::Connection> session = connect_another(clock());
    ASSERT_TRUE(session.has_value());
    const Result<Posted> attempt = post_committed(*session, trade());
    ASSERT_FALSE(attempt.has_value());
    EXPECT_EQ(attempt.error().code(), ErrorCode::Unavailable);
    EXPECT_FALSE(session->connected());
    EXPECT_EQ(snapshot(), before);
    arm(0, false, false);
    const Result<Posted> retried = post_committed(trade());
    ASSERT_TRUE(retried.has_value());
    EXPECT_FALSE(retried->replayed);
    expect_applied_once(before);
}

// Tiến trình gọi chết sau khi post xong mà trước commit: một phiên khác ngắt kết nối của nó.
TEST_F(LedgerFaults, ACrashBetweenPostAndCommitRollsBack) {
    const State before = snapshot();
    Result<db::Connection> session = connect_another(clock());
    ASSERT_TRUE(session.has_value());
    const Result<db::Rows> pid = session->execute("SELECT pg_backend_pid()::int8", {}, deadline());
    ASSERT_TRUE(pid.has_value());
    const Result<i64> backend = pid->integer(0, 0);
    ASSERT_TRUE(backend.has_value());
    {
        Result<db::Transaction> transaction =
            session->begin(db::Isolation::ReadCommitted, deadline());
        ASSERT_TRUE(transaction.has_value());
        ASSERT_TRUE(post(*transaction, trade()).has_value());
        // Chờ tới khi backend đã thoát hẳn (tham số timeout, PostgreSQL 14 trở lên), để COMMIT chắc
        // chắn tới một kết nối đã chết.
        const std::array params{db::Param::integer(*backend)};
        const Result<db::Rows> killed = connection().execute(
            "SELECT pg_terminate_backend($1::int4, 60000)", params, deadline());
        ASSERT_TRUE(killed.has_value()) << connection().last_error();
        const Result<void> committed = transaction->commit();
        ASSERT_FALSE(committed.has_value());
        EXPECT_EQ(committed.error().code(), ErrorCode::Unavailable);
    }
    EXPECT_EQ(snapshot(), before);
    expect_clean("sau khi phiên chết trước commit");
    const Result<Posted> retried = post_committed(trade());
    ASSERT_TRUE(retried.has_value());
    EXPECT_FALSE(retried->replayed);
    expect_applied_once(before);
}

// COMMIT thành công nhưng phản hồi không tới bên gọi: bên gọi retry, và mọi lần retry chỉ thấy
// bút toán đã áp.
TEST_F(LedgerFaults, ALostCommitResponseIsResolvedByRetry) {
    const State before = snapshot();
    const Result<Posted> committed = post_committed(trade());
    ASSERT_TRUE(committed.has_value());
    EXPECT_FALSE(committed->replayed);
    const Result<Posted> retried = post_committed(trade());
    ASSERT_TRUE(retried.has_value());
    EXPECT_TRUE(retried->replayed);
    EXPECT_EQ(retried->entry, committed->entry);
    expect_applied_once(before);
}

}  // namespace
}  // namespace orion::ledger
