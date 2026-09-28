#include "game/server/lib/ledger/ledger.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/narrow.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/value.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace orion::ledger {
namespace {

constexpr std::string_view kDigestTag = "orion.ledger.entry.v1";

// Tài khoản khác nhau tối đa trong một bút toán: hai đầu của mỗi dòng và mỗi lượt.
constexpr usize kMaxAccounts = 2 * (kMaxTransfers + kMaxItemMoves);
// Số dư bị đổi tối đa trong một bút toán: hai đầu của mỗi dòng chuyển.
constexpr usize kMaxDeltas = 2 * kMaxTransfers;
// Tài khoản mỗi câu tra loại: câu viết sẵn đúng 32 tham số, trong giới hạn của server_db.
constexpr usize kLookupBatch = 32;
static_assert(kLookupBatch <= db::kMaxParams);
// Số dòng mỗi loại ghi vào một u8 của digest.
static_assert(kMaxTransfers <= 255 && kMaxItemMoves <= 255);

[[nodiscard]] bool known_reason(const Reason reason) noexcept {
    switch (reason) {
        case Reason::Grant:
        case Reason::Purchase:
        case Reason::Loot:
        case Reason::Trade:
        case Reason::Vendor:
        case Reason::Consume:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_code(const std::string_view code) noexcept {
    if (code.empty() || code.size() > kMaxCodeBytes || code.front() < 'a' || code.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(code, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

[[nodiscard]] Result<void> check_transfer(const Transfer& transfer) noexcept {
    if (!transfer.from.valid() || !transfer.to.valid() || !transfer.asset.valid()) {
        return fail(ErrorCode::InvalidArgument, "ledger: dòng chuyển có id không hợp lệ");
    }
    if (transfer.from == transfer.to) {
        return fail(ErrorCode::InvalidArgument, "ledger: dòng chuyển có hai đầu trùng nhau",
                    transfer.from.value);
    }
    if (transfer.amount < 1 || transfer.amount > kMaxAmount) {
        return fail(ErrorCode::OutOfRange, "ledger: số lượng ngoài [1, 10^15]", transfer.amount);
    }
    return {};
}

[[nodiscard]] Result<void> check_move(const std::span<const ItemMove> moves,
                                      const usize index) noexcept {
    const ItemMove& move = moves[index];
    if (!move.item.valid() || !move.asset.valid() || !move.from.valid() || !move.to.valid()) {
        return fail(ErrorCode::InvalidArgument, "ledger: lượt chuyển vật phẩm có id không hợp lệ");
    }
    if (move.from == move.to) {
        return fail(ErrorCode::InvalidArgument, "ledger: lượt chuyển có hai đầu trùng nhau",
                    move.item.value);
    }
    const bool repeated = std::ranges::any_of(
        moves.first(index), [&move](const ItemMove& other) { return other.item == move.item; });
    if (repeated) {
        return fail(ErrorCode::InvalidArgument, "ledger: vật phẩm có hai lượt trong bút toán",
                    move.item.value);
    }
    return {};
}

// Bốn số i64 little-endian: một dòng hay một lượt trong digest.
void hash_row(crypto::Hasher& hasher, const std::array<i64, 4>& fields) noexcept {
    std::array<std::byte, 32> row{};
    for (usize i = 0; i < fields.size(); ++i) {
        core::store_le<i64>(std::span(row).subspan(i * 8).first<8>(), fields[i]);
    }
    hasher.update(row);
}

// ---- Tra loại tài khoản ------------------------------------------------------------------------

struct KnownAccount {
    AccountId id;
    u8 kind = 0;  // 0 khi chưa đọc; sau lookup là giá trị của AccountKind
};

// Các tài khoản khác nhau của một bút toán, sắp tăng dần. Không cấp phát.
class AccountSet {
public:
    explicit AccountSet(const Entry& entry) noexcept {
        for (const Transfer& transfer : entry.transfers) {
            add(transfer.from);
            add(transfer.to);
        }
        for (const ItemMove& move : entry.items) {
            add(move.from);
            add(move.to);
        }
        const std::span all = std::span(items_).first(size_);
        std::ranges::sort(all, {}, &KnownAccount::id);
        size_ = static_cast<usize>(std::ranges::unique(all, {}, &KnownAccount::id).begin() -
                                   all.begin());
    }

    [[nodiscard]] std::span<KnownAccount> all() noexcept { return std::span(items_).first(size_); }

    // Loại của một tài khoản trong tập, sau khi lookup thành công. VERIFY chứ không ASSERT: sai thì
    // đọc ngoài phần đã tra và ghi sổ theo loại rác.
    [[nodiscard]] AccountKind kind(const AccountId id) const noexcept {
        const std::span known = std::span(items_).first(size_);
        const auto found = std::ranges::lower_bound(known, id, {}, &KnownAccount::id);
        ORION_VERIFY(found != known.end() && found->id == id && found->kind != 0,
                     "ledger: tài khoản {} chưa được tra", id.value);
        return static_cast<AccountKind>(found->kind);
    }

private:
    void add(const AccountId id) noexcept {
        ORION_VERIFY(size_ < items_.size(), "ledger: quá nhiều tài khoản");
        items_[size_++] = KnownAccount{.id = id, .kind = 0};
    }

    std::array<KnownAccount, kMaxAccounts> items_{};
    usize size_ = 0;
};

template <usize... I>
[[nodiscard]] std::array<db::Param, sizeof...(I)> repeated(
    const db::Param& value, std::index_sequence<I...> /*slots*/) noexcept {
    return {((void)I, value)...};
}

// Đọc loại của tối đa 32 tài khoản bằng một câu. Chỗ trống là id 0, thứ không tài khoản nào có.
[[nodiscard]] Result<void> lookup_batch(db::Transaction& transaction,
                                        const std::span<KnownAccount> batch) noexcept {
    std::array params = repeated(db::Param::integer(0), std::make_index_sequence<kLookupBatch>{});
    for (usize i = 0; i < batch.size(); ++i) {
        params.at(i) = db::Param::integer(batch[i].id.value);
    }
    const Result<db::Rows> rows = transaction.execute(
        "SELECT id, kind FROM ledger_accounts WHERE id IN ($1, $2, $3, $4, $5, $6, $7, $8, $9, "
        "$10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, $26, "
        "$27, $28, $29, $30, $31, $32)",
        params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    for (usize row = 0; row < rows->size(); ++row) {
        const Result<i64> id = rows->integer(row, 0);
        if (!id) {
            return std::unexpected(id.error());
        }
        const Result<i64> kind = rows->integer(row, 1);
        if (!kind) {
            return std::unexpected(kind.error());
        }
        const auto found = std::ranges::find(batch, AccountId{*id}, &KnownAccount::id);
        if (found == batch.end() ||
            (std::cmp_not_equal(*kind, std::to_underlying(AccountKind::Holder)) &&
             std::cmp_not_equal(*kind, std::to_underlying(AccountKind::External)))) {
            return fail(ErrorCode::DataLoss, "ledger: DB trả tài khoản lạ khi tra loại", *id);
        }
        found->kind = static_cast<u8>(*kind);
    }
    return {};
}

[[nodiscard]] Result<void> lookup(db::Transaction& transaction, AccountSet& accounts) noexcept {
    const std::span<KnownAccount> all = accounts.all();
    for (usize begin = 0; begin < all.size(); begin += kLookupBatch) {
        const usize count = std::min(kLookupBatch, all.size() - begin);
        if (const Result<void> read = lookup_batch(transaction, all.subspan(begin, count)); !read) {
            return read;
        }
    }
    const auto missing = std::ranges::find(all, u8{0}, &KnownAccount::kind);
    if (missing != all.end()) {
        return fail(ErrorCode::NotFound, "ledger: tài khoản không có", missing->id.value);
    }
    return {};
}

// Dòng hay lượt giữa hai tài khoản ngoài không giữ gì cho ai: từ chối trước khi ghi.
[[nodiscard]] Result<void> check_kinds(const Entry& entry, const AccountSet& accounts) noexcept {
    const auto both_external = [&accounts](const AccountId from, const AccountId to) {
        return accounts.kind(from) == AccountKind::External &&
               accounts.kind(to) == AccountKind::External;
    };
    for (const Transfer& transfer : entry.transfers) {
        if (both_external(transfer.from, transfer.to)) {
            return fail(ErrorCode::FailedPrecondition,
                        "ledger: dòng chuyển giữa hai tài khoản ngoài", transfer.from.value);
        }
    }
    for (const ItemMove& move : entry.items) {
        if (both_external(move.from, move.to)) {
            return fail(ErrorCode::FailedPrecondition,
                        "ledger: lượt chuyển giữa hai tài khoản ngoài", move.item.value);
        }
    }
    return {};
}

// ---- Ghi
// -----------------------------------------------------------------------------------------

// Thêm dòng bút toán; nullopt khi khoá đã có.
[[nodiscard]] Result<std::optional<EntryId>> insert_entry(db::Transaction& transaction,
                                                          const Entry& entry,
                                                          const crypto::Hash& hash) noexcept {
    const std::array params{db::Param::bytes(entry.key),
                            db::Param::integer(std::to_underlying(entry.reason)),
                            db::Param::bytes(hash.view())};
    const Result<db::Rows> rows = transaction.execute(
        "INSERT INTO ledger_entries (idempotency_key, reason, digest) "
        "VALUES ($1, $2, $3) ON CONFLICT (idempotency_key) DO NOTHING "
        "RETURNING id",
        params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->size() == 0) {
        return std::nullopt;
    }
    const Result<i64> id = rows->integer(0, 0);
    if (!id) {
        return std::unexpected(id.error());
    }
    return EntryId{*id};
}

// Khoá đã có: cùng digest là một lần trước đã áp đúng bút toán này.
[[nodiscard]] Result<Posted> replay(db::Transaction& transaction, const Entry& entry,
                                    const crypto::Hash& hash) noexcept {
    const std::array params{db::Param::bytes(entry.key)};
    const Result<db::Rows> rows = transaction.execute(
        "SELECT id, digest FROM ledger_entries WHERE idempotency_key = $1", params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    // ON CONFLICT DO NOTHING vừa thấy khoá này đã commit, hay do chính transaction này thêm.
    if (rows->size() != 1) {
        return fail(ErrorCode::Internal, "ledger: khoá vừa trùng mà không đọc lại được");
    }
    const Result<i64> id = rows->integer(0, 0);
    if (!id) {
        return std::unexpected(id.error());
    }
    const Result<std::span<const std::byte>> stored = rows->bytes(0, 1);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (!crypto::equal_constant_time(*stored, hash.view())) {
        return fail(ErrorCode::AlreadyExists, "ledger: khoá đã dùng cho bút toán khác", *id);
    }
    return Posted{.entry = EntryId{*id}, .replayed = true};
}

struct Delta {
    AccountId account;
    AssetId asset;
    i64 amount = 0;
};

// Thay đổi ròng của mỗi cặp (tài khoản giữ, tài sản), sắp tăng dần theo cặp: thứ tự khoá cố định.
// |tổng| ≤ 32 · 10^15, không tràn i64.
class Deltas {
public:
    Deltas(const Entry& entry, const AccountSet& accounts) noexcept {
        for (const Transfer& transfer : entry.transfers) {
            if (accounts.kind(transfer.from) == AccountKind::Holder) {
                add({.account = transfer.from,
                     .asset = transfer.asset,
                     .amount = -transfer.amount});
            }
            if (accounts.kind(transfer.to) == AccountKind::Holder) {
                add({.account = transfer.to, .asset = transfer.asset, .amount = transfer.amount});
            }
        }
        const std::span all = std::span(items_).first(size_);
        std::ranges::sort(all, {}, [](const Delta& d) { return std::pair(d.account, d.asset); });
        usize merged = 0;
        for (const Delta& delta : all) {
            const bool same = merged > 0 && items_[merged - 1].account == delta.account &&
                              items_[merged - 1].asset == delta.asset;
            if (same) {
                items_[merged - 1].amount += delta.amount;
            } else {
                items_[merged++] = delta;
            }
        }
        size_ = merged;
    }

    [[nodiscard]] std::span<const Delta> all() const noexcept {
        return std::span(items_).first(size_);
    }

private:
    void add(const Delta& delta) noexcept {
        ORION_VERIFY(size_ < items_.size(), "ledger: quá nhiều số dư bị đổi");
        items_[size_++] = delta;
    }

    std::array<Delta, kMaxDeltas> items_{};
    usize size_ = 0;
};

[[nodiscard]] Result<void> apply_delta(db::Transaction& transaction, const Delta& delta) noexcept {
    const bool debit = delta.amount < 0;
    const std::array params{db::Param::integer(delta.account.value),
                            db::Param::integer(delta.asset.value),
                            db::Param::integer(debit ? -delta.amount : delta.amount)};
    // Trừ: điều kiện đủ số dư nằm trong chính câu UPDATE, nên được kiểm lại trên bản mới nhất của
    // dòng khi phải chờ transaction khác (ReadCommitted).
    const Result<db::Rows> rows =
        debit ? transaction.execute(
                    "UPDATE ledger_balances SET balance = balance - $3 "
                    "WHERE account_id = $1 AND asset = $2 AND balance >= $3",
                    params)
              : transaction.execute(
                    "INSERT INTO ledger_balances (account_id, asset, balance) VALUES ($1, $2, $3) "
                    "ON CONFLICT (account_id, asset) "
                    "DO UPDATE SET balance = ledger_balances.balance + EXCLUDED.balance",
                    params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (debit && rows->affected() != 1) {
        return fail(ErrorCode::FailedPrecondition, "ledger: không đủ số dư", delta.account.value);
    }
    return {};
}

[[nodiscard]] Result<void> apply_balances(db::Transaction& transaction, const Entry& entry,
                                          const AccountSet& accounts) noexcept {
    const Deltas deltas(entry, accounts);
    for (const Delta& delta : deltas.all()) {
        if (delta.amount == 0) {
            continue;
        }
        if (const Result<void> applied = apply_delta(transaction, delta); !applied) {
            return applied;
        }
    }
    return {};
}

// Lượt chuyển từ tài khoản giữ không áp được: đọc lại vật phẩm để nói vì sao.
[[nodiscard]] std::unexpected<Error> move_refused(db::Transaction& transaction,
                                                  const ItemMove& move) noexcept {
    const std::array params{db::Param::integer(move.item.value)};
    const Result<db::Rows> rows =
        transaction.execute("SELECT account_id, asset FROM ledger_items WHERE id = $1", params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->size() == 0) {
        return fail(ErrorCode::NotFound, "ledger: vật phẩm không có", move.item.value);
    }
    const Result<i64> owner = rows->integer(0, 0);
    if (!owner) {
        return std::unexpected(owner.error());
    }
    const Result<i64> asset = rows->integer(0, 1);
    if (!asset) {
        return std::unexpected(asset.error());
    }
    if (*owner != move.from.value) {
        return fail(ErrorCode::FailedPrecondition, "ledger: vật phẩm không ở tài khoản nguồn",
                    move.item.value);
    }
    if (*asset != move.asset.value) {
        return fail(ErrorCode::FailedPrecondition, "ledger: vật phẩm khác định nghĩa",
                    move.item.value);
    }
    // Giữa câu UPDATE và câu đọc lại, một transaction khác vừa trả vật phẩm về đúng chủ.
    return fail(ErrorCode::Aborted, "ledger: vật phẩm vừa đổi chủ, chạy lại", move.item.value);
}

// Tạo vật phẩm (từ tài khoản ngoài) hay đổi chủ nó (từ tài khoản giữ); trả version mới.
[[nodiscard]] Result<i64> move_item(db::Transaction& transaction, const ItemMove& move,
                                    const AccountKind from_kind) noexcept {
    if (from_kind == AccountKind::External) {
        const std::array params{db::Param::integer(move.item.value),
                                db::Param::integer(move.asset.value),
                                db::Param::integer(move.to.value)};
        const Result<db::Rows> rows = transaction.execute(
            "INSERT INTO ledger_items (id, asset, account_id, version) "
            "VALUES ($1, $2, $3, 1) ON CONFLICT (id) DO NOTHING "
            "RETURNING version",
            params);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        if (rows->size() != 1) {
            return fail(ErrorCode::AlreadyExists, "ledger: vật phẩm đã có", move.item.value);
        }
        return rows->integer(0, 0);
    }
    const std::array params{db::Param::integer(move.item.value),
                            db::Param::integer(move.asset.value),
                            db::Param::integer(move.from.value), db::Param::integer(move.to.value)};
    const Result<db::Rows> rows = transaction.execute(
        "UPDATE ledger_items SET account_id = $4, version = version + 1 "
        "WHERE id = $1 AND asset = $2 AND account_id = $3 RETURNING version",
        params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->size() != 1) {
        return move_refused(transaction, move);
    }
    return rows->integer(0, 0);
}

[[nodiscard]] Result<void> apply_items(db::Transaction& transaction, const Entry& entry,
                                       const AccountSet& accounts, const EntryId id) noexcept {
    std::array<u8, kMaxItemMoves> order{};
    const std::span indices = std::span(order).first(entry.items.size());
    for (usize i = 0; i < indices.size(); ++i) {
        indices[i] = static_cast<u8>(i);
    }
    std::ranges::sort(indices, {}, [&entry](const u8 i) { return entry.items[i].item; });
    for (const u8 i : indices) {
        const ItemMove& move = entry.items[i];
        const Result<i64> version = move_item(transaction, move, accounts.kind(move.from));
        if (!version) {
            return std::unexpected(version.error());
        }
        const std::array params{
            db::Param::integer(id.value),        db::Param::integer(i),
            db::Param::integer(move.item.value), db::Param::integer(*version),
            db::Param::integer(move.from.value), db::Param::integer(move.to.value)};
        const Result<db::Rows> recorded = transaction.execute(
            "INSERT INTO ledger_item_moves "
            "(entry_id, ordinal, item_id, version, from_account, to_account) "
            "VALUES ($1, $2, $3, $4, $5, $6)",
            params);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
    }
    return {};
}

[[nodiscard]] Result<void> record_transfers(db::Transaction& transaction, const Entry& entry,
                                            const EntryId id) noexcept {
    for (usize i = 0; i < entry.transfers.size(); ++i) {
        const Transfer& transfer = entry.transfers[i];
        const std::array params{db::Param::integer(id.value),
                                db::Param::integer(narrow<i64>(i)),
                                db::Param::integer(transfer.from.value),
                                db::Param::integer(transfer.to.value),
                                db::Param::integer(transfer.asset.value),
                                db::Param::integer(transfer.amount)};
        const Result<db::Rows> recorded = transaction.execute(
            "INSERT INTO ledger_transfers "
            "(entry_id, ordinal, from_account, to_account, asset, amount) "
            "VALUES ($1, $2, $3, $4, $5, $6)",
            params);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
    }
    return {};
}

// Tài khoản có mã vừa bị một lần tạo khác giành trước: đọc nó và kiểm loại.
[[nodiscard]] Result<AccountId> existing_account(db::Transaction& transaction,
                                                 const AccountKind kind,
                                                 const std::string_view code) noexcept {
    const std::array params{db::Param::text(code)};
    const Result<db::Rows> rows =
        transaction.execute("SELECT id, kind FROM ledger_accounts WHERE code = $1", params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->size() != 1) {
        return fail(ErrorCode::Internal, "ledger: mã vừa trùng mà không đọc lại được");
    }
    const Result<i64> id = rows->integer(0, 0);
    if (!id) {
        return std::unexpected(id.error());
    }
    const Result<i64> stored = rows->integer(0, 1);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (std::cmp_not_equal(*stored, std::to_underlying(kind))) {
        return fail(ErrorCode::FailedPrecondition, "ledger: mã đã thuộc tài khoản loại khác", *id);
    }
    return AccountId{*id};
}

// Số đếm không âm ở cột `column` của dòng đầu.
[[nodiscard]] Result<u64> count_at(const db::Rows& rows, const usize column) noexcept {
    const Result<i64> count = rows.integer(0, column);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count < 0) {
        return fail(ErrorCode::DataLoss, "ledger: DB trả số đếm âm", *count);
    }
    return static_cast<u64>(*count);
}

}  // namespace

Result<void> validate(const Entry& entry) noexcept {
    if (entry.key.empty() || entry.key.size() > kMaxKeyBytes) {
        return fail(ErrorCode::InvalidArgument, "ledger: khoá idempotency phải 1 tới 64 byte",
                    static_cast<i64>(entry.key.size()));
    }
    if (!known_reason(entry.reason)) {
        return fail(ErrorCode::InvalidArgument, "ledger: lý do không có trong bảng",
                    std::to_underlying(entry.reason));
    }
    if (entry.transfers.size() > kMaxTransfers || entry.items.size() > kMaxItemMoves) {
        return fail(ErrorCode::InvalidArgument, "ledger: bút toán quá 32 dòng mỗi loại");
    }
    if (entry.transfers.empty() && entry.items.empty()) {
        return fail(ErrorCode::InvalidArgument, "ledger: bút toán không có dòng nào");
    }
    for (const Transfer& transfer : entry.transfers) {
        if (const Result<void> checked = check_transfer(transfer); !checked) {
            return checked;
        }
    }
    for (usize i = 0; i < entry.items.size(); ++i) {
        if (const Result<void> checked = check_move(entry.items, i); !checked) {
            return checked;
        }
    }
    return {};
}

crypto::Hash digest(const Entry& entry) noexcept {
    ORION_ASSERT(entry.transfers.size() <= kMaxTransfers && entry.items.size() <= kMaxItemMoves,
                 "ledger: digest của bút toán chưa kiểm");
    crypto::Hasher hasher;
    hasher.update(std::as_bytes(std::span(kDigestTag)));
    const std::array head{static_cast<std::byte>(std::to_underlying(entry.reason)),
                          static_cast<std::byte>(entry.transfers.size())};
    hasher.update(head);
    for (const Transfer& transfer : entry.transfers) {
        hash_row(hasher,
                 {transfer.from.value, transfer.to.value, transfer.asset.value, transfer.amount});
    }
    const std::array items{static_cast<std::byte>(entry.items.size())};
    hasher.update(items);
    for (const ItemMove& move : entry.items) {
        hash_row(hasher, {move.item.value, move.asset.value, move.from.value, move.to.value});
    }
    return hasher.finish();
}

Result<AccountId> open_holder(db::Transaction& transaction) noexcept {
    const Result<db::Rows> rows =
        transaction.execute("INSERT INTO ledger_accounts (kind) VALUES (1) RETURNING id");
    if (!rows) {
        return std::unexpected(rows.error());
    }
    const Result<i64> id = rows->integer(0, 0);
    if (!id) {
        return std::unexpected(id.error());
    }
    return AccountId{*id};
}

Result<AccountId> named_account(db::Transaction& transaction, const AccountKind kind,
                                const std::string_view code) noexcept {
    if (!valid_code(code)) {
        return fail(ErrorCode::InvalidArgument, "ledger: mã tài khoản sai dạng",
                    static_cast<i64>(code.size()));
    }
    const std::array params{db::Param::integer(std::to_underlying(kind)), db::Param::text(code)};
    const Result<db::Rows> created = transaction.execute(
        "INSERT INTO ledger_accounts (kind, code) VALUES ($1, $2) "
        "ON CONFLICT (code) DO NOTHING RETURNING id",
        params);
    if (!created) {
        return std::unexpected(created.error());
    }
    if (created->size() == 0) {
        return existing_account(transaction, kind, code);
    }
    const Result<i64> id = created->integer(0, 0);
    if (!id) {
        return std::unexpected(id.error());
    }
    return AccountId{*id};
}

Result<Posted> post(db::Transaction& transaction, const Entry& entry) noexcept {
    if (const Result<void> valid = validate(entry); !valid) {
        return std::unexpected(valid.error());
    }
    const crypto::Hash hash = digest(entry);
    AccountSet accounts(entry);
    if (const Result<void> found = lookup(transaction, accounts); !found) {
        return std::unexpected(found.error());
    }
    if (const Result<void> kinds = check_kinds(entry, accounts); !kinds) {
        return std::unexpected(kinds.error());
    }
    const Result<std::optional<EntryId>> inserted = insert_entry(transaction, entry, hash);
    if (!inserted) {
        return std::unexpected(inserted.error());
    }
    if (!inserted->has_value()) {
        return replay(transaction, entry, hash);
    }
    const EntryId id = **inserted;
    if (const Result<void> applied = apply_balances(transaction, entry, accounts); !applied) {
        return std::unexpected(applied.error());
    }
    if (const Result<void> moved = apply_items(transaction, entry, accounts, id); !moved) {
        return std::unexpected(moved.error());
    }
    if (const Result<void> recorded = record_transfers(transaction, entry, id); !recorded) {
        return std::unexpected(recorded.error());
    }
    return Posted{.entry = id, .replayed = false};
}

Result<i64> balance(db::Transaction& transaction, const AccountId account,
                    const AssetId asset) noexcept {
    const std::array params{db::Param::integer(account.value), db::Param::integer(asset.value)};
    const Result<db::Rows> rows = transaction.execute(
        "SELECT a.kind, coalesce(b.balance, 0) FROM ledger_accounts a "
        "LEFT JOIN ledger_balances b ON b.account_id = a.id AND b.asset = $2 WHERE a.id = $1",
        params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->size() == 0) {
        return fail(ErrorCode::NotFound, "ledger: tài khoản không có", account.value);
    }
    const Result<i64> kind = rows->integer(0, 0);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (std::cmp_not_equal(*kind, std::to_underlying(AccountKind::Holder))) {
        return fail(ErrorCode::FailedPrecondition, "ledger: tài khoản ngoài không có số dư",
                    account.value);
    }
    return rows->integer(0, 1);
}

Result<std::optional<ItemState>> item(db::Transaction& transaction, const ItemId id) noexcept {
    const std::array params{db::Param::integer(id.value)};
    const Result<db::Rows> rows = transaction.execute(
        "SELECT account_id, asset, version FROM ledger_items WHERE id = $1", params);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    if (rows->size() == 0) {
        return std::nullopt;
    }
    std::array<i64, 3> values{};
    for (usize column = 0; column < values.size(); ++column) {
        const Result<i64> value = rows->integer(0, column);
        if (!value) {
            return std::unexpected(value.error());
        }
        values.at(column) = *value;
    }
    return ItemState{
        .owner = AccountId{values[0]}, .asset = AssetId{values[1]}, .version = values[2]};
}

Result<Audit> audit(db::Transaction& transaction) noexcept {
    const Result<db::Rows> rows = transaction.execute(
        // Số dư của tài khoản giữ khác tổng nhật ký, kể cả cặp có nhật ký mà không có dòng số dư.
        "SELECT (SELECT count(*) FROM ("
        "  SELECT f.account, f.asset, sum(f.delta) AS total FROM ("
        "    SELECT to_account AS account, asset, amount::numeric AS delta FROM ledger_transfers"
        "    UNION ALL"
        "    SELECT from_account, asset, -amount::numeric FROM ledger_transfers) f"
        "  JOIN ledger_accounts a ON a.id = f.account AND a.kind = 1"
        "  GROUP BY f.account, f.asset) s"
        "  FULL JOIN ledger_balances b ON b.account_id = s.account AND b.asset = s.asset"
        "  WHERE coalesce(s.total, 0) <> coalesce(b.balance, 0)),"
        // Chủ của vật phẩm khác đích của lượt có cùng version (kể cả khi lượt đó không có).
        " (SELECT count(*) FROM ledger_items i"
        "  LEFT JOIN ledger_item_moves m ON m.item_id = i.id AND m.version = i.version"
        "  WHERE m.to_account IS DISTINCT FROM i.account_id),"
        // Chuỗi đứt: lượt tạo không từ tài khoản ngoài, lượt sau không từ đích của lượt trước,
        // hay lượt có version vượt version của vật phẩm.
        " (SELECT count(*) FROM ledger_item_moves m"
        "  JOIN ledger_accounts src ON src.id = m.from_account"
        "  JOIN ledger_items i ON i.id = m.item_id"
        "  LEFT JOIN ledger_item_moves p ON p.item_id = m.item_id AND p.version = m.version - 1"
        "  WHERE (m.version = 1 AND src.kind <> 2)"
        "     OR (m.version > 1 AND p.to_account IS DISTINCT FROM m.from_account)"
        "     OR m.version > i.version)");
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::array<u64, 3> counts{};
    for (usize column = 0; column < counts.size(); ++column) {
        const Result<u64> count = count_at(*rows, column);
        if (!count) {
            return std::unexpected(count.error());
        }
        counts.at(column) = *count;
    }
    return Audit{
        .balance_mismatches = counts[0], .owner_mismatches = counts[1], .broken_chains = counts[2]};
}

}  // namespace orion::ledger
