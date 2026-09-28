#pragma once

// Fixture cho test cần ledger trên PostgreSQL thật: schema riêng của PostgresTest đã áp mọi
// migration của repo, và các tiện ích ghi, đọc sổ trong transaction riêng. Dùng cho test của
// ledger, và của các dịch vụ ghi sổ (store, persistence).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/tests/support/postgres_test.hpp"
#include "game/server/lib/ledger/ledger.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace orion::ledger::testing {

// Byte của một chuỗi, làm khoá idempotency trong test. Chuỗi phải sống lâu hơn span.
[[nodiscard]] std::span<const std::byte> key(std::string_view text) noexcept;

class LedgerTest : public db::testing::PostgresTest {
protected:
    void SetUp() override;

    // Chạy fn(transaction) trong một transaction trên `connection`, rồi commit. fn trả Result; lỗi
    // thì transaction rollback và lỗi đi nguyên lên.
    template <class F>
    [[nodiscard]] auto in_transaction(db::Connection& connection, F&& fn,
                                      const db::Isolation isolation = db::Isolation::ReadCommitted)
        -> decltype(fn(std::declval<db::Transaction&>())) {
        Result<db::Transaction> transaction = connection.begin(isolation, deadline());
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        auto result = std::forward<F>(fn)(*transaction);
        if (!result) {
            return result;
        }
        if (const Result<void> committed = transaction->commit(); !committed) {
            return std::unexpected(committed.error());
        }
        return result;
    }

    // post trong một transaction riêng, rồi commit.
    [[nodiscard]] Result<Posted> post_committed(
        db::Connection& connection, const Entry& entry,
        db::Isolation isolation = db::Isolation::ReadCommitted);
    [[nodiscard]] Result<Posted> post_committed(const Entry& entry);

    // Đọc và ghi trên kết nối của fixture, mỗi lời gọi một transaction.
    [[nodiscard]] Result<i64> balance_of(AccountId account, AssetId asset);
    [[nodiscard]] Result<std::optional<ItemState>> item_of(ItemId id);
    [[nodiscard]] Result<Audit> audit_now();
    [[nodiscard]] Result<AccountId> new_holder();
    [[nodiscard]] Result<AccountId> account_named(AccountKind kind, std::string_view code);
    // Số đếm của một câu SELECT count(*), ví dụ số dòng của một bảng nhật ký.
    [[nodiscard]] Result<i64> count(db::Sql sql);
};

}  // namespace orion::ledger::testing
