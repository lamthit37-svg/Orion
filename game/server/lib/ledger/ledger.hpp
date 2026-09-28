#pragma once

// Ledger ghi kép của kinh tế (CLAUDE.md X.9; hợp đồng ở docs/formats/ledger.md): mọi thay đổi về
// vật phẩm và tiền là một bút toán ghi trong transaction của bên gọi, kèm khoá idempotency. Đây là
// nơi duy nhất ghi các bảng ledger_*; store và persistence cùng gọi code này (ADR 0008).
//
// - Mọi hàm chạy trong một db::Transaction do bên gọi mở và commit, nên bút toán đi cùng các ghi
//   khác của bên gọi (đơn hàng của store) trong một transaction. Khuyên dùng ReadCommitted.
// - post lỗi thì transaction không dùng tiếp được: bên gọi rollback. Commit lỗi Unavailable,
//   DeadlineExceeded hay Aborted thì bên gọi chạy lại cả transaction với đúng bút toán và khoá cũ:
//   bút toán đã áp thì post trả replayed, chưa thì áp đúng một lần.
// - crypto::initialize() phải đã thành công: digest của bút toán là BLAKE2b.
// - Luồng: như db::Transaction, một luồng tại một thời điểm.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/hash.hpp"
#include "game/server/lib/db/db.hpp"

#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace orion::ledger {

// Ghi ra đĩa: giá trị không bao giờ đổi (X.3).
enum class AccountKind : u8 {
    Holder = 1,    // giữ tài sản cho ai đó (ví, túi đồ, ký quỹ): số dư không âm
    External = 2,  // nguồn hay đích của kinh tế: không có số dư, luôn có mã
};

// Vì sao bút toán xảy ra. Ghi ra đĩa: giá trị không bao giờ đổi, chỉ thêm giá trị mới (X.3).
enum class Reason : u8 {
    Grant = 1,     // vận hành cấp hay thu hồi: GM, bồi thường
    Purchase = 2,  // mua trong store
    Loot = 3,      // rơi đồ, rương, phần thưởng
    Trade = 4,     // trao đổi giữa hai người chơi
    Vendor = 5,    // mua bán với NPC
    Consume = 6,   // dùng, phá vật phẩm; phí
};

struct AccountId {
    i64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr auto operator<=>(AccountId, AccountId) noexcept = default;
};

// Id của định nghĩa tiền hay vật phẩm trong data/.
struct AssetId {
    i64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr auto operator<=>(AssetId, AssetId) noexcept = default;
};

// Id bền của một vật phẩm (ADR 0006).
struct ItemId {
    i64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr auto operator<=>(ItemId, ItemId) noexcept = default;
};

struct EntryId {
    i64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr auto operator<=>(EntryId, EntryId) noexcept = default;
};

// Giới hạn của một bút toán (ledger.md, mục "Giới hạn"); CHECK của migration khớp các số này.
inline constexpr usize kMaxKeyBytes = 64;
inline constexpr usize kMaxTransfers = 32;
inline constexpr usize kMaxItemMoves = 32;
inline constexpr i64 kMaxAmount = 1'000'000'000'000'000;
inline constexpr usize kMaxCodeBytes = 63;

// `amount` đơn vị `asset` đi từ `from` sang `to`.
struct Transfer {
    AccountId from{};
    AccountId to{};
    AssetId asset{};
    i64 amount = 0;
};

// Vật phẩm `item` (định nghĩa `asset`) đi từ `from` sang `to`. Từ tài khoản ngoài là tạo nó, vào
// tài khoản ngoài là huỷ nó.
struct ItemMove {
    ItemId item{};
    AssetId asset{};
    AccountId from{};
    AccountId to{};
};

// Một bút toán. Chỉ trỏ vào bộ nhớ của bên gọi, phải sống tới khi post trả về. Mọi trường có giá
// trị mặc định để khởi tạo chỉ định bỏ được trường không dùng: thiếu `{}`, clang cảnh báo trường
// bị bỏ (-Wmissing-designated-field-initializers), nên `{}` của span không thừa như clang-tidy
// nghĩ.
struct Entry {
    std::span<const std::byte> key{};  // NOLINT(readability-redundant-member-init): xem trên
    Reason reason = Reason::Grant;
    std::span<const Transfer> transfers{};  // NOLINT(readability-redundant-member-init): như trên
    std::span<const ItemMove> items{};      // NOLINT(readability-redundant-member-init): như trên
};

// Kiểm bút toán theo mục "Giới hạn", không chạm DB. Lỗi: InvalidArgument; OutOfRange khi amount
// ngoài [1, kMaxAmount].
[[nodiscard]] Result<void> validate(const Entry& entry) noexcept;

// Digest của nội dung bút toán, trừ khoá (ledger.md, mục "Digest"). Tiền điều kiện: validate thành
// công.
[[nodiscard]] crypto::Hash digest(const Entry& entry) noexcept;

// Mở một tài khoản giữ không mã, ví dụ ví của một nhân vật mới.
[[nodiscard]] Result<AccountId> open_holder(db::Transaction& transaction) noexcept;

// Tài khoản có mã `code` (ledger.md, "Mã tài khoản"): trả tài khoản đã có, hay tạo mới với loại
// `kind`. Lỗi: InvalidArgument khi mã sai dạng; FailedPrecondition khi mã đã thuộc tài khoản loại
// khác.
[[nodiscard]] Result<AccountId> named_account(db::Transaction& transaction, AccountKind kind,
                                              std::string_view code) noexcept;

struct Posted {
    EntryId entry{};
    // true: khoá đã được áp bởi một lần trước với cùng nội dung; lần này không áp gì.
    bool replayed = false;
};

// Ghi một bút toán (ledger.md, mục "Ghi một bút toán"). Lỗi: như validate; NotFound (tài khoản hay
// vật phẩm không có, detail là id); FailedPrecondition (không đủ số dư, detail là id tài khoản;
// vật phẩm khác chủ hay khác định nghĩa, detail là id vật phẩm; dòng giữa hai tài khoản ngoài);
// AlreadyExists (tạo vật phẩm có id đã có, detail là id; khoá đã dùng cho nội dung khác, detail là
// id bút toán cũ); OutOfRange (số dư tràn int8); và lỗi của server_db. Mọi lỗi để lại transaction
// không dùng tiếp được.
[[nodiscard]] Result<Posted> post(db::Transaction& transaction, const Entry& entry) noexcept;

// Số dư của một tài khoản giữ; 0 khi chưa từng có. Lỗi: NotFound khi tài khoản không có;
// FailedPrecondition khi là tài khoản ngoài, thứ không có số dư.
[[nodiscard]] Result<i64> balance(db::Transaction& transaction, AccountId account,
                                  AssetId asset) noexcept;

struct ItemState {
    AccountId owner{};
    AssetId asset{};
    i64 version = 0;
};

// Chủ hiện tại của một vật phẩm; nullopt khi vật phẩm chưa từng được tạo.
[[nodiscard]] Result<std::optional<ItemState>> item(db::Transaction& transaction,
                                                    ItemId id) noexcept;

// Kết quả soát sổ (ledger.md, mục "Soát").
struct Audit {
    u64 balance_mismatches = 0;
    u64 owner_mismatches = 0;
    u64 broken_chains = 0;

    [[nodiscard]] constexpr bool clean() const noexcept {
        return balance_mismatches == 0 && owner_mismatches == 0 && broken_chains == 0;
    }
};

// Soát toàn bộ sổ, O(số dòng của nhật ký). Cho công cụ vận hành và test, không cho đường request.
[[nodiscard]] Result<Audit> audit(db::Transaction& transaction) noexcept;

}  // namespace orion::ledger
