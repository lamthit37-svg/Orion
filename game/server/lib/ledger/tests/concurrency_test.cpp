// Hai phiên ghi sổ cùng lúc (ledger.md, mục "Mức cô lập"). Phiên thứ nhất giữ khoá dòng trong một
// transaction chưa commit; phiên thứ hai chạy trên luồng riêng (engine/jobs, CLAUDE.md X.7) và
// phải chờ. Luồng test hỏi pg_locks cho tới khi thấy phiên thứ hai đang chờ khoá, rồi mới commit
// hay rollback phiên thứ nhất: thứ tự xen kẽ vì vậy cố định, không nhờ sleep (X.4).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/value.hpp"
#include "game/server/lib/ledger/ledger.hpp"
#include "game/server/lib/ledger/tests/support/ledger_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace orion::ledger {
namespace {

using testing::key;
using testing::LedgerTest;

constexpr AssetId kGold{1};
// Số lần hỏi pg_locks tối đa: mỗi lần là một lượt đi về tới server, nên đây là vài giây, dư cho
// phiên thứ hai gửi câu của nó và bắt đầu chờ.
constexpr int kMaxPolls = 200'000;

// Kết quả của một luồng trước khi nó ghi kết quả thật.
[[nodiscard]] Result<Posted> not_run() {
    return fail(ErrorCode::Internal, "test: luồng chưa ghi kết quả");
}

class LedgerConcurrency : public LedgerTest {
protected:
    void SetUp() override {
        LedgerTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        const Result<AccountId> grant = account_named(AccountKind::External, "grant");
        const Result<AccountId> alice = new_holder();
        const Result<AccountId> bob = new_holder();
        const Result<AccountId> carol = new_holder();
        ASSERT_TRUE(grant && alice && bob && carol);
        grant_ = *grant;
        alice_ = *alice;
        bob_ = *bob;
        carol_ = *carol;
        const std::array seed{Transfer{grant_, alice_, kGold, 100}};
        ASSERT_TRUE(
            post_committed({.key = key("seed"), .reason = Reason::Grant, .transfers = seed}));
        Result<db::Connection> first = connect_another(clock());
        Result<db::Connection> second = connect_another(clock());
        ASSERT_TRUE(first.has_value() && second.has_value());
        first_ = *std::move(first);
        second_ = *std::move(second);
        first_pid_ = backend_pid(first_);
        second_pid_ = backend_pid(second_);
    }

    // true khi phiên `pid` đang chờ một khoá chưa được cấp; false khi luồng của nó đã xong (`done`)
    // mà không phải chờ, hay hết số lần hỏi.
    [[nodiscard]] bool waiting(const i64 pid, const std::atomic<bool>& done) {
        const std::array params{db::Param::integer(pid)};
        for (int poll = 0; poll < kMaxPolls && !done.load(std::memory_order_acquire); ++poll) {
            const Result<db::Rows> rows = connection().execute(
                "SELECT count(*) FROM pg_locks WHERE pid = $1::int4 AND NOT granted", params,
                deadline());
            if (!rows) {
                ADD_FAILURE() << connection().last_error();
                return false;
            }
            const Result<i64> waiting = rows->integer(0, 0);
            if (waiting && *waiting > 0) {
                return true;
            }
        }
        return false;
    }

    // Chạy post_committed(entry) của phiên thứ hai trên một luồng; kết quả đọc sau join.
    [[nodiscard]] Result<jobs::Thread> start_second(
        const Entry& entry, const db::Isolation isolation = db::Isolation::ReadCommitted) {
        return jobs::Thread::start("ledger-second", [this, entry, isolation](jobs::StopToken) {
            second_result_ = post_committed(second_, entry, isolation);
            // release: luồng test thấy second_done_ thì cũng thấy second_result_.
            second_done_.store(true, std::memory_order_release);
        });
    }

    // true khi phiên thứ hai đang chờ khoá.
    [[nodiscard]] bool second_waiting() { return waiting(second_pid_, second_done_); }

    [[nodiscard]] i64 gold(const AccountId account) {
        const Result<i64> value = balance_of(account, kGold);
        EXPECT_TRUE(value.has_value());
        return value ? *value : -1;
    }

    [[nodiscard]] AccountId grant() const noexcept { return grant_; }
    [[nodiscard]] AccountId alice() const noexcept { return alice_; }
    [[nodiscard]] AccountId bob() const noexcept { return bob_; }
    [[nodiscard]] AccountId carol() const noexcept { return carol_; }
    [[nodiscard]] db::Connection& first() noexcept { return first_; }
    [[nodiscard]] db::Connection& second() noexcept { return second_; }
    [[nodiscard]] i64 first_pid() const noexcept { return first_pid_; }
    // Kết quả của start_second; chỉ đọc sau join.
    [[nodiscard]] const Result<Posted>& second_result() const noexcept { return second_result_; }

private:
    [[nodiscard]] i64 backend_pid(db::Connection& session) {
        const Result<db::Rows> pid =
            session.execute("SELECT pg_backend_pid()::int8", {}, deadline());
        EXPECT_TRUE(pid.has_value());
        const Result<i64> number = pid ? pid->integer(0, 0) : Result<i64>(0);
        EXPECT_TRUE(number.has_value());
        return number ? *number : 0;
    }

    AccountId grant_;
    AccountId alice_;
    AccountId bob_;
    AccountId carol_;
    // Chưa kết nối cho tới SetUp; đồng hồ là của PostgresTest, dựng trước các thành viên này.
    db::Connection first_{clock()};
    db::Connection second_{clock()};
    i64 first_pid_ = 0;
    i64 second_pid_ = 0;
    // Chỉ luồng của phiên thứ hai ghi, trước khi đặt second_done_; luồng test đọc sau join.
    Result<Posted> second_result_ = not_run();
    std::atomic<bool> second_done_{false};
};

// Hai lần tiêu hết cùng 100 vàng: lần sau chờ lần trước, rồi kiểm lại số dư trên bản mới nhất và
// bị từ chối.
TEST_F(LedgerConcurrency, TwoSpendsOfOneBalanceNeverBothSucceed) {
    const std::array to_b{Transfer{alice(), bob(), kGold, 100}};
    const std::array to_c{Transfer{alice(), carol(), kGold, 100}};
    Result<db::Transaction> transaction = first().begin(db::Isolation::ReadCommitted, deadline());
    ASSERT_TRUE(transaction.has_value());
    ASSERT_TRUE(
        post(*transaction, {.key = key("to-b"), .reason = Reason::Trade, .transfers = to_b}));
    Result<jobs::Thread> thread =
        start_second({.key = key("to-c"), .reason = Reason::Trade, .transfers = to_c});
    ASSERT_TRUE(thread.has_value());
    // Không ASSERT trước join: thoát sớm khi phiên thứ nhất còn giữ khoá thì luồng kia chờ mãi.
    const bool waited = second_waiting();
    const bool committed = transaction->commit().has_value();
    thread->join();
    EXPECT_TRUE(waited);
    EXPECT_TRUE(committed);
    ASSERT_FALSE(second_result().has_value());
    EXPECT_EQ(second_result().error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(gold(alice()), 0);
    EXPECT_EQ(gold(bob()), 100);
    EXPECT_EQ(gold(carol()), 0);
}

// Hai lần thử cùng một khoá chạy xen nhau (retry khi lần đầu tưởng đã hỏng): lần sau chờ lần
// trước; lần trước commit thì lần sau trả replayed.
TEST_F(LedgerConcurrency, ConcurrentAttemptsOfOneKeyApplyOnce) {
    const std::array bonus{Transfer{grant(), bob(), kGold, 7}};
    const Entry entry{.key = key("bonus"), .reason = Reason::Grant, .transfers = bonus};
    Result<db::Transaction> transaction = first().begin(db::Isolation::ReadCommitted, deadline());
    ASSERT_TRUE(transaction.has_value());
    const Result<Posted> first = post(*transaction, entry);
    ASSERT_TRUE(first.has_value());
    Result<jobs::Thread> thread = start_second(entry);
    ASSERT_TRUE(thread.has_value());
    const bool waited = second_waiting();
    const bool committed = transaction->commit().has_value();
    thread->join();
    EXPECT_TRUE(waited);
    EXPECT_TRUE(committed);
    ASSERT_TRUE(second_result().has_value());
    EXPECT_TRUE(second_result()->replayed);
    EXPECT_EQ(second_result()->entry, first->entry);
    EXPECT_EQ(gold(bob()), 7);
}

// Lần trước rollback (tiến trình của nó chết trước commit): lần sau, đang chờ, tự áp bút toán.
TEST_F(LedgerConcurrency, TheWaitingAttemptAppliesWhenTheFirstRollsBack) {
    const std::array bonus{Transfer{grant(), bob(), kGold, 7}};
    const Entry entry{.key = key("bonus"), .reason = Reason::Grant, .transfers = bonus};
    Result<db::Transaction> transaction = first().begin(db::Isolation::ReadCommitted, deadline());
    ASSERT_TRUE(transaction.has_value());
    ASSERT_TRUE(post(*transaction, entry).has_value());
    Result<jobs::Thread> thread = start_second(entry);
    ASSERT_TRUE(thread.has_value());
    const bool waited = second_waiting();
    const bool rolled_back = transaction->rollback().has_value();
    thread->join();
    EXPECT_TRUE(waited);
    EXPECT_TRUE(rolled_back);
    ASSERT_TRUE(second_result().has_value());
    EXPECT_FALSE(second_result()->replayed);
    EXPECT_EQ(gold(bob()), 7);
    const Result<Audit> report = audit_now();
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->clean());
}

// Transaction ghi nhiều bút toán có thể deadlock (ledger.md, mục "Ghi một bút toán"): mỗi phiên
// giữ số dư của một người rồi đòi số dư của người kia. PostgreSQL huỷ đúng một bên với Aborted, bên
// kia xong; bên bị huỷ chạy lại cả transaction với cùng khoá thì áp.
TEST_F(LedgerConcurrency, ADeadlockAcrossEntriesAbortsExactlyOneSide) {
    const Result<AccountId> sink = account_named(AccountKind::External, "sink");
    ASSERT_TRUE(sink.has_value());
    const std::array seed_b{Transfer{grant(), bob(), kGold, 100}};
    ASSERT_TRUE(
        post_committed({.key = key("seed-b"), .reason = Reason::Grant, .transfers = seed_b}));
    const std::array a_pays{Transfer{alice(), *sink, kGold, 1}};
    const std::array b_pays{Transfer{bob(), *sink, kGold, 1}};
    const auto entry = [](const std::string_view name, const std::span<const Transfer> lines) {
        return Entry{.key = key(name), .reason = Reason::Vendor, .transfers = lines};
    };
    Result<db::Transaction> one = first().begin(db::Isolation::ReadCommitted, deadline());
    Result<db::Transaction> two = second().begin(db::Isolation::ReadCommitted, deadline());
    ASSERT_TRUE(one.has_value() && two.has_value());
    ASSERT_TRUE(post(*one, entry("one-a", a_pays)).has_value());
    ASSERT_TRUE(post(*two, entry("two-b", b_pays)).has_value());

    Result<Posted> one_result = not_run();
    bool one_rolled_back = true;
    std::atomic<bool> one_done{false};
    Result<jobs::Thread> thread = jobs::Thread::start("ledger-first", [&](jobs::StopToken) {
        Result<Posted> result = post(*one, entry("one-b", b_pays));
        if (!result) {
            // Bị chọn làm bên huỷ: trả khoá ngay để bên kia xong.
            one_rolled_back = one->rollback().has_value();
        }
        one_result = std::move(result);
        one_done.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(thread.has_value());
    const bool waited = waiting(first_pid(), one_done);
    const Result<Posted> two_result = post(*two, entry("two-a", a_pays));
    const bool two_rolled_back = two_result || two->rollback().has_value();
    thread->join();
    EXPECT_TRUE(waited);
    EXPECT_TRUE(one_rolled_back && two_rolled_back);
    ASSERT_NE(one_result.has_value(), two_result.has_value()) << "đúng một bên bị huỷ";
    const Error& victim = one_result ? two_result.error() : one_result.error();
    EXPECT_EQ(victim.code(), ErrorCode::Aborted);

    // Bên còn lại commit; bên bị huỷ chạy lại cả transaction của nó.
    const bool first_won = one_result.has_value();
    EXPECT_TRUE((first_won ? *one : *two).commit().has_value());
    db::Connection& loser = first_won ? second() : first();
    const Result<Posted> redone = in_transaction(loser, [&](db::Transaction& transaction) {
        const Result<Posted> head =
            post(transaction, first_won ? entry("two-b", b_pays) : entry("one-a", a_pays));
        return head ? post(transaction, first_won ? entry("two-a", a_pays) : entry("one-b", b_pays))
                    : head;
    });
    ASSERT_TRUE(redone.has_value());
    EXPECT_FALSE(redone->replayed);
    EXPECT_EQ(gold(alice()), 98);
    EXPECT_EQ(gold(bob()), 98);
    const Result<Audit> report = audit_now();
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->clean());
}

// Ở RepeatableRead, lần thử sau thấy khoá do một transaction commit sau snapshot của nó: PostgreSQL
// báo xung đột (40001, Aborted) thay vì trả replayed; chạy lại cả transaction thì thấy replayed.
TEST_F(LedgerConcurrency, UnderRepeatableReadTheConflictIsAborted) {
    const std::array bonus{Transfer{grant(), bob(), kGold, 7}};
    const Entry entry{.key = key("bonus"), .reason = Reason::Grant, .transfers = bonus};
    Result<db::Transaction> transaction = first().begin(db::Isolation::ReadCommitted, deadline());
    ASSERT_TRUE(transaction.has_value());
    ASSERT_TRUE(post(*transaction, entry).has_value());
    Result<jobs::Thread> thread = start_second(entry, db::Isolation::RepeatableRead);
    ASSERT_TRUE(thread.has_value());
    const bool waited = second_waiting();
    const bool committed = transaction->commit().has_value();
    thread->join();
    EXPECT_TRUE(waited);
    EXPECT_TRUE(committed);
    ASSERT_FALSE(second_result().has_value());
    EXPECT_EQ(second_result().error().code(), ErrorCode::Aborted);
    EXPECT_EQ(second_result().error().detail(), db::kSerializationFailure);

    const Result<Posted> retried =
        in_transaction(second(), [&entry](db::Transaction& again) { return post(again, entry); });
    ASSERT_TRUE(retried.has_value());
    EXPECT_TRUE(retried->replayed);
    EXPECT_EQ(gold(bob()), 7);
}

// Thứ tự khoá cố định (ledger.md, mục "Ghi một bút toán"): A trả B và B trả A cùng lúc không
// deadlock. Một trigger chỉ có trong test giữ phiên thứ nhất lại ngay sau khi nó khoá dòng số dư
// đầu tiên, bằng cách chờ một advisory lock mà luồng test đang nắm; phiên thứ hai chạy bút toán
// chiều ngược lại trong lúc đó. Theo thứ tự tăng dần, phiên thứ hai cũng đòi dòng của A trước nên
// chỉ chờ; theo thứ tự của các dòng chuyển, nó sẽ khoá B trước và hai phiên chờ nhau.
TEST_F(LedgerConcurrency, OppositeTransfersNeverDeadlock) {
    const std::array seed_b{Transfer{grant(), bob(), kGold, 100}};
    ASSERT_TRUE(
        post_committed({.key = key("seed-b"), .reason = Reason::Grant, .transfers = seed_b}));
    ASSERT_TRUE(connection()
                    .execute_script(std::format(R"sql(
CREATE FUNCTION pause_first() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF pg_backend_pid() = {} THEN
        PERFORM pg_advisory_lock(4242);
        PERFORM pg_advisory_unlock(4242);
    END IF;
    RETURN NULL;
END
$$;
CREATE TRIGGER pause_first AFTER INSERT OR UPDATE ON ledger_balances
    FOR EACH ROW EXECUTE FUNCTION pause_first();
SELECT pg_advisory_lock(4242);
)sql",
                                                first_pid()),
                                    deadline())
                    .has_value())
        << connection().last_error();

    const std::array alice_pays{Transfer{alice(), bob(), kGold, 10}};
    const std::array bob_pays{Transfer{bob(), alice(), kGold, 20}};
    Result<Posted> first_result = not_run();
    std::atomic<bool> first_done{false};
    Result<jobs::Thread> first_thread = jobs::Thread::start("ledger-first", [&](jobs::StopToken) {
        first_result = post_committed(
            first(), {.key = key("alice"), .reason = Reason::Trade, .transfers = alice_pays});
        first_done.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(first_thread.has_value());
    const bool first_paused = waiting(first_pid(), first_done);
    Result<jobs::Thread> second_thread =
        start_second({.key = key("bob"), .reason = Reason::Trade, .transfers = bob_pays});
    const bool second_waited = second_thread.has_value() && second_waiting();
    const Result<db::Rows> released =
        connection().execute("SELECT pg_advisory_unlock(4242)", {}, deadline());
    first_thread->join();
    if (second_thread) {
        second_thread->join();
    }
    ASSERT_TRUE(released.has_value());
    EXPECT_TRUE(first_paused);
    EXPECT_TRUE(second_waited);
    EXPECT_TRUE(first_result.has_value()) << (first_result ? "" : first_result.error().context());
    EXPECT_TRUE(second_result().has_value())
        << (second_result() ? "" : second_result().error().context());
    EXPECT_EQ(gold(alice()), 110);
    EXPECT_EQ(gold(bob()), 90);
}

// Lượt chuyển của phiên thứ nhất không khớp chủ, và giữa câu UPDATE với câu đọc lại để nói vì sao,
// một phiên khác trả vật phẩm về đúng chủ mà lượt đó nêu. Không thể nói là sai chủ nữa: lỗi là
// Aborted, bên gọi chạy lại. Trigger mức câu lệnh (chạy cả khi UPDATE không đổi dòng nào) giữ phiên
// thứ nhất lại ngay sau câu UPDATE.
TEST_F(LedgerConcurrency, AnItemReturnedMidPostIsAborted) {
    constexpr ItemId kRing{77};
    constexpr AssetId kRingDefinition{300};
    const std::array to_carol{ItemMove{kRing, kRingDefinition, grant(), carol()}};
    ASSERT_TRUE(post_committed({.key = key("ring"), .reason = Reason::Loot, .items = to_carol}));
    ASSERT_TRUE(connection()
                    .execute_script(std::format(R"sql(
CREATE FUNCTION pause_items() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF pg_backend_pid() = {} THEN
        PERFORM pg_advisory_lock(4243);
        PERFORM pg_advisory_unlock(4243);
    END IF;
    RETURN NULL;
END
$$;
CREATE TRIGGER pause_items AFTER UPDATE ON ledger_items
    FOR EACH STATEMENT EXECUTE FUNCTION pause_items();
SELECT pg_advisory_lock(4243);
)sql",
                                                first_pid()),
                                    deadline())
                    .has_value())
        << connection().last_error();

    const std::array stale{ItemMove{kRing, kRingDefinition, alice(), bob()}};
    Result<Posted> first_result = not_run();
    std::atomic<bool> first_done{false};
    Result<jobs::Thread> thread = jobs::Thread::start("ledger-first", [&](jobs::StopToken) {
        first_result =
            post_committed(first(), {.key = key("stale"), .reason = Reason::Trade, .items = stale});
        first_done.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(thread.has_value());
    const bool paused = waiting(first_pid(), first_done);
    const std::array to_alice{ItemMove{kRing, kRingDefinition, carol(), alice()}};
    const Result<Posted> returned =
        post_committed({.key = key("return"), .reason = Reason::Trade, .items = to_alice});
    const Result<db::Rows> released =
        connection().execute("SELECT pg_advisory_unlock(4243)", {}, deadline());
    thread->join();
    EXPECT_TRUE(paused);
    ASSERT_TRUE(returned.has_value());
    ASSERT_TRUE(released.has_value());
    ASSERT_FALSE(first_result.has_value());
    EXPECT_EQ(first_result.error().code(), ErrorCode::Aborted);
    EXPECT_EQ(first_result.error().detail(), kRing.value);
    const Result<Audit> report = audit_now();
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->clean());
}

}  // namespace
}  // namespace orion::ledger
