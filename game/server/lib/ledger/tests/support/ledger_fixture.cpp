#include "game/server/lib/ledger/tests/support/ledger_fixture.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/tests/support/postgres_test.hpp"
#include "game/server/lib/ledger/ledger.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace orion::ledger::testing {

std::span<const std::byte> key(const std::string_view text) noexcept {
    return std::as_bytes(std::span(text));
}

void LedgerTest::SetUp() {
    PostgresTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    ASSERT_TRUE(crypto::initialize().has_value());
    const Result<void> migrated =
        db::testing::apply_repository_migrations(connection(), deadline());
    ASSERT_TRUE(migrated.has_value()) << db::testing::describe(migrated.error(), connection());
}

Result<Posted> LedgerTest::post_committed(db::Connection& connection, const Entry& entry,
                                          const db::Isolation isolation) {
    return in_transaction(
        connection, [&entry](db::Transaction& transaction) { return post(transaction, entry); },
        isolation);
}

Result<Posted> LedgerTest::post_committed(const Entry& entry) {
    return post_committed(connection(), entry);
}

Result<i64> LedgerTest::balance_of(const AccountId account, const AssetId asset) {
    return in_transaction(connection(), [&](db::Transaction& transaction) {
        return balance(transaction, account, asset);
    });
}

Result<std::optional<ItemState>> LedgerTest::item_of(const ItemId id) {
    return in_transaction(connection(),
                          [id](db::Transaction& transaction) { return item(transaction, id); });
}

Result<Audit> LedgerTest::audit_now() {
    return in_transaction(connection(),
                          [](db::Transaction& transaction) { return audit(transaction); });
}

Result<AccountId> LedgerTest::new_holder() {
    return in_transaction(connection(),
                          [](db::Transaction& transaction) { return open_holder(transaction); });
}

Result<AccountId> LedgerTest::account_named(const AccountKind kind, const std::string_view code) {
    return in_transaction(connection(), [&](db::Transaction& transaction) {
        return named_account(transaction, kind, code);
    });
}

Result<i64> LedgerTest::count(const db::Sql sql) {
    return in_transaction(connection(), [sql](db::Transaction& transaction) -> Result<i64> {
        const Result<db::Rows> rows = transaction.execute(sql);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        return rows->integer(0, 0);
    });
}

}  // namespace orion::ledger::testing
