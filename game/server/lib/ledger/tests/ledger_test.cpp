#include "game/server/lib/ledger/ledger.hpp"

#include "engine/core/error.hpp"
#include "engine/core/narrow.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/ledger/tests/support/ledger_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::ledger {
namespace {

using testing::key;
using testing::LedgerTest;

constexpr AssetId kGold{1};
constexpr AssetId kGem{2};
constexpr AssetId kSword{100};
constexpr AssetId kShield{101};

// Cùng dòng, với trường thứ `field` (theo thứ tự trong digest) cộng thêm 100.
[[nodiscard]] Transfer bumped(Transfer transfer, const usize field) {
    switch (field) {
        case 0:
            transfer.from.value += 100;
            break;
        case 1:
            transfer.to.value += 100;
            break;
        case 2:
            transfer.asset.value += 100;
            break;
        default:
            transfer.amount += 100;
            break;
    }
    return transfer;
}

[[nodiscard]] ItemMove bumped(ItemMove move, const usize field) {
    switch (field) {
        case 0:
            move.item.value += 100;
            break;
        case 1:
            move.asset.value += 100;
            break;
        case 2:
            move.from.value += 100;
            break;
        default:
            move.to.value += 100;
            break;
    }
    return move;
}

// Lỗi dạng chữ cho thông điệp khi test đỏ.
[[nodiscard]] std::string why(const Error& error) {
    return std::format("{}", error);
}

[[nodiscard]] std::string hex(const crypto::Hash& hash) {
    std::string text;
    for (const std::byte b : hash.bytes) {
        text += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return text;
}

// ---- Kiểm bút toán, không cần DB -----------------------------------------------------------

TEST(LedgerEntry, WellFormedEntriesPass) {
    const std::array transfers{Transfer{AccountId{1}, AccountId{2}, kGold, 1},
                               Transfer{AccountId{2}, AccountId{3}, kGem, kMaxAmount}};
    const std::array items{ItemMove{ItemId{9}, kSword, AccountId{1}, AccountId{2}}};
    const std::string longest(kMaxKeyBytes, 'k');
    EXPECT_TRUE(
        validate({.key = key("k"), .reason = Reason::Trade, .transfers = transfers}).has_value());
    EXPECT_TRUE(
        validate({.key = key(longest), .reason = Reason::Loot, .items = items}).has_value());
    EXPECT_TRUE(
        validate(
            {.key = key("k"), .reason = Reason::Consume, .transfers = transfers, .items = items})
            .has_value());
    std::vector<Transfer> many(kMaxTransfers, transfers[0]);
    std::vector<ItemMove> distinct;
    for (i64 i = 1; std::cmp_less_equal(i, kMaxItemMoves); ++i) {
        distinct.push_back(ItemMove{ItemId{i}, kSword, AccountId{1}, AccountId{2}});
    }
    EXPECT_TRUE(
        validate({.key = key("k"), .reason = Reason::Grant, .transfers = many, .items = distinct})
            .has_value());
}

struct Malformed {
    std::string_view what;
    Entry entry;
    ErrorCode code;
};

TEST(LedgerEntry, MalformedEntriesAreRejected) {
    const std::array ok{Transfer{AccountId{1}, AccountId{2}, kGold, 5}};
    const std::array self{Transfer{AccountId{1}, AccountId{1}, kGold, 5}};
    const std::array no_from{Transfer{AccountId{0}, AccountId{2}, kGold, 5}};
    const std::array negative_to{Transfer{AccountId{1}, AccountId{-2}, kGold, 5}};
    const std::array no_asset{Transfer{AccountId{1}, AccountId{2}, AssetId{0}, 5}};
    const std::array zero{Transfer{AccountId{1}, AccountId{2}, kGold, 0}};
    const std::array negative{Transfer{AccountId{1}, AccountId{2}, kGold, -5}};
    const std::array huge{Transfer{AccountId{1}, AccountId{2}, kGold, kMaxAmount + 1}};
    const std::array no_item{ItemMove{ItemId{0}, kSword, AccountId{1}, AccountId{2}}};
    const std::array item_no_asset{ItemMove{ItemId{3}, AssetId{0}, AccountId{1}, AccountId{2}}};
    const std::array item_no_to{ItemMove{ItemId{3}, kSword, AccountId{1}, AccountId{0}}};
    const std::array item_self{ItemMove{ItemId{3}, kSword, AccountId{1}, AccountId{1}}};
    const std::array twice{ItemMove{ItemId{3}, kSword, AccountId{1}, AccountId{2}},
                           ItemMove{ItemId{4}, kSword, AccountId{1}, AccountId{2}},
                           ItemMove{ItemId{3}, kSword, AccountId{2}, AccountId{5}}};
    const std::vector<Transfer> too_many(kMaxTransfers + 1, ok[0]);
    std::vector<ItemMove> too_many_items;
    for (i64 i = 1; std::cmp_less_equal(i, kMaxItemMoves + 1); ++i) {
        too_many_items.push_back(ItemMove{ItemId{i}, kSword, AccountId{1}, AccountId{2}});
    }
    const std::string too_long(kMaxKeyBytes + 1, 'k');
    const auto entry = [&](const std::span<const Transfer> transfers,
                           const std::span<const ItemMove> items = {}) {
        return Entry{
            .key = key("k"), .reason = Reason::Trade, .transfers = transfers, .items = items};
    };
    const std::array cases{
        Malformed{"khoá rỗng", Entry{.key = {}, .transfers = ok}, ErrorCode::InvalidArgument},
        Malformed{"khoá 65 byte", Entry{.key = key(too_long), .transfers = ok},
                  ErrorCode::InvalidArgument},
        Malformed{"lý do 0", Entry{.key = key("k"), .reason = Reason{0}, .transfers = ok},
                  ErrorCode::InvalidArgument},
        Malformed{"lý do 7", Entry{.key = key("k"), .reason = Reason{7}, .transfers = ok},
                  ErrorCode::InvalidArgument},
        Malformed{"không có dòng", entry({}), ErrorCode::InvalidArgument},
        Malformed{"33 dòng chuyển", entry(too_many), ErrorCode::InvalidArgument},
        Malformed{"33 lượt", entry({}, too_many_items), ErrorCode::InvalidArgument},
        Malformed{"from 0", entry(no_from), ErrorCode::InvalidArgument},
        Malformed{"to âm", entry(negative_to), ErrorCode::InvalidArgument},
        Malformed{"asset 0", entry(no_asset), ErrorCode::InvalidArgument},
        Malformed{"hai đầu trùng", entry(self), ErrorCode::InvalidArgument},
        Malformed{"số lượng 0", entry(zero), ErrorCode::OutOfRange},
        Malformed{"số lượng âm", entry(negative), ErrorCode::OutOfRange},
        Malformed{"số lượng 10^15 + 1", entry(huge), ErrorCode::OutOfRange},
        Malformed{"vật phẩm 0", entry({}, no_item), ErrorCode::InvalidArgument},
        Malformed{"định nghĩa 0", entry({}, item_no_asset), ErrorCode::InvalidArgument},
        Malformed{"lượt tới 0", entry({}, item_no_to), ErrorCode::InvalidArgument},
        Malformed{"lượt hai đầu trùng", entry({}, item_self), ErrorCode::InvalidArgument},
        Malformed{"vật phẩm hai lượt", entry({}, twice), ErrorCode::InvalidArgument},
    };
    for (const Malformed& c : cases) {
        const Result<void> checked = validate(c.entry);
        ASSERT_FALSE(checked.has_value()) << c.what;
        EXPECT_EQ(checked.error().code(), c.code) << c.what;
    }
    EXPECT_EQ(validate(entry({}, twice)).error().detail(), 3);
    EXPECT_EQ(validate(entry(huge)).error().detail(), kMaxAmount + 1);
}

// Vector kiểm tính độc lập bằng hashlib.blake2b(digest_size=32) của Python trên đúng mã hoá của
// ledger.md, mục "Digest".
TEST(LedgerEntry, DigestFollowsTheSpecifiedEncoding) {
    ASSERT_TRUE(crypto::initialize().has_value());
    const std::array trade{Transfer{AccountId{7}, AccountId{9}, AssetId{1001}, 250},
                           Transfer{AccountId{9}, AccountId{7}, AssetId{1002}, 3}};
    const std::array traded{
        ItemMove{ItemId{500'000'000'001}, AssetId{2001}, AccountId{7}, AccountId{9}}};
    EXPECT_EQ(hex(digest(
                  {.key = key("a"), .reason = Reason::Trade, .transfers = trade, .items = traded})),
              "26d1c11783dc804514a757a313456922d0b78654a31056ddb21f9f3f5a6fef4f");
    const std::array grant{Transfer{AccountId{3}, AccountId{4}, AssetId{1}, 1}};
    EXPECT_EQ(hex(digest({.key = key("b"), .reason = Reason::Grant, .transfers = grant})),
              "ecd1988af026ab49c46f74948dfc46d8b7c689901a205602650efbbcadd7b621");
    const std::array loot{ItemMove{ItemId{12}, AssetId{34}, AccountId{5}, AccountId{6}}};
    EXPECT_EQ(hex(digest({.key = key("c"), .reason = Reason::Loot, .items = loot})),
              "bc15ff9507ae8e799051f3153621d13373fd4399b716bcf6cb2647862b1ff7cd");
}

TEST(LedgerEntry, DigestCoversEveryFieldButTheKey) {
    ASSERT_TRUE(crypto::initialize().has_value());
    const std::array transfers{Transfer{AccountId{1}, AccountId{2}, kGold, 10},
                               Transfer{AccountId{2}, AccountId{3}, kGem, 20}};
    const std::array items{ItemMove{ItemId{5}, kSword, AccountId{1}, AccountId{2}}};
    const Entry base{
        .key = key("x"), .reason = Reason::Trade, .transfers = transfers, .items = items};
    const std::string reference = hex(digest(base));
    Entry other_key = base;
    other_key.key = key("y");
    EXPECT_EQ(hex(digest(other_key)), reference);

    std::vector<std::string> seen{reference};
    const auto differs = [&seen](const Entry& changed, const std::string_view what) {
        const std::string value = hex(digest(changed));
        EXPECT_EQ(std::ranges::count(seen, value), 0) << what;
        seen.push_back(value);
    };
    Entry reason = base;
    reason.reason = Reason::Vendor;
    differs(reason, "lý do");
    for (usize field = 0; field < 4; ++field) {
        const std::array changed{transfers[0], bumped(transfers[1], field)};
        differs({.key = key("x"), .reason = Reason::Trade, .transfers = changed, .items = items},
                "trường của dòng chuyển");
        const std::array moved{bumped(items[0], field)};
        differs({.key = key("x"), .reason = Reason::Trade, .transfers = transfers, .items = moved},
                "trường của lượt chuyển");
    }
    const std::array swapped{transfers[1], transfers[0]};
    differs({.key = key("x"), .reason = Reason::Trade, .transfers = swapped, .items = items},
            "thứ tự dòng");
    // Một dòng chuyển thành lượt chuyển vật phẩm với cùng bốn số: số dòng mỗi loại nằm trong
    // digest.
    const std::array as_item{ItemMove{ItemId{1}, AssetId{2}, AccountId{1}, AccountId{10}}};
    const std::array as_transfer{Transfer{AccountId{1}, AccountId{2}, AssetId{1}, 10}};
    EXPECT_NE(hex(digest({.key = key("x"), .reason = Reason::Trade, .items = as_item})),
              hex(digest({.key = key("x"), .reason = Reason::Trade, .transfers = as_transfer})));
}

// ---- Trên PostgreSQL thật ------------------------------------------------------------------

class Ledger : public LedgerTest {
protected:
    void SetUp() override {
        LedgerTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        const Result<AccountId> grant = account_named(AccountKind::External, "grant");
        const Result<AccountId> sink = account_named(AccountKind::External, "sink");
        const Result<AccountId> alice = new_holder();
        const Result<AccountId> bob = new_holder();
        ASSERT_TRUE(grant && sink && alice && bob);
        grant_ = *grant;
        sink_ = *sink;
        alice_ = *alice;
        bob_ = *bob;
    }

    // Cấp `amount` tài sản cho `to` từ tài khoản ngoài "grant", khoá riêng mỗi lần gọi.
    void give(const AccountId to, const AssetId asset, const i64 amount) {
        const std::string name = std::format("give-{}", ++gifts_);
        const std::array transfers{Transfer{grant_, to, asset, amount}};
        const Result<Posted> posted =
            post_committed({.key = key(name), .reason = Reason::Grant, .transfers = transfers});
        ASSERT_TRUE(posted.has_value()) << why(posted.error());
    }

    // Tạo vật phẩm cho `to` từ "grant".
    void create(const ItemId id, const AssetId asset, const AccountId to) {
        const std::string name = std::format("create-{}", id.value);
        const std::array items{ItemMove{id, asset, grant(), to}};
        const Result<Posted> posted =
            post_committed({.key = key(name), .reason = Reason::Loot, .items = items});
        ASSERT_TRUE(posted.has_value()) << why(posted.error());
    }

    [[nodiscard]] i64 balance_or_minus_one(const AccountId account, const AssetId asset) {
        const Result<i64> value = balance_of(account, asset);
        EXPECT_TRUE(value.has_value()) << why(value.error());
        return value.value_or(-1);
    }

    // Trạng thái của một vật phẩm đã có; ItemState rỗng (và test đỏ) khi không có.
    [[nodiscard]] ItemState state_of(const ItemId id) {
        const Result<std::optional<ItemState>> state = item_of(id);
        EXPECT_TRUE(state.has_value() && state->has_value());
        return state && *state ? **state : ItemState{};
    }

    [[nodiscard]] AccountId owner_of(const ItemId id) { return state_of(id).owner; }

    void expect_clean() {
        const Result<Audit> report = audit_now();
        ASSERT_TRUE(report.has_value()) << why(report.error());
        EXPECT_TRUE(report->clean()) << report->balance_mismatches << ' '
                                     << report->owner_mismatches << ' ' << report->broken_chains;
    }

    [[nodiscard]] AccountId grant() const noexcept { return grant_; }
    [[nodiscard]] AccountId sink() const noexcept { return sink_; }
    [[nodiscard]] AccountId alice() const noexcept { return alice_; }
    [[nodiscard]] AccountId bob() const noexcept { return bob_; }

private:
    AccountId grant_;
    AccountId sink_;
    AccountId alice_;
    AccountId bob_;
    int gifts_ = 0;
};

TEST_F(Ledger, GrantCreditsAHolderAndRecordsTheEntry) {
    const std::array transfers{Transfer{grant(), alice(), kGold, 100}};
    const Result<Posted> posted =
        post_committed({.key = key("g1"), .reason = Reason::Grant, .transfers = transfers});
    ASSERT_TRUE(posted.has_value()) << why(posted.error());
    EXPECT_TRUE(posted->entry.valid());
    EXPECT_FALSE(posted->replayed);
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 100);
    EXPECT_EQ(balance_or_minus_one(alice(), kGem), 0);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_entries"), 1);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_transfers"), 1);
    // Tài khoản ngoài không có dòng số dư: nguồn không bị mọi giao dịch cùng tranh khoá.
    EXPECT_EQ(count("SELECT count(*) FROM ledger_balances"), 1);
    expect_clean();
}

TEST_F(Ledger, TransferBetweenHoldersMovesTheBalance) {
    give(alice(), kGold, 100);
    const std::array transfers{Transfer{alice(), bob(), kGold, 30}};
    ASSERT_TRUE(
        post_committed({.key = key("t1"), .reason = Reason::Trade, .transfers = transfers}));
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 70);
    EXPECT_EQ(balance_or_minus_one(bob(), kGold), 30);
    expect_clean();
}

TEST_F(Ledger, SpendingMoreThanTheBalanceLeavesNothing) {
    give(alice(), kGold, 70);
    const std::array transfers{Transfer{alice(), sink(), kGold, 71}};
    const Result<Posted> posted =
        post_committed({.key = key("spend"), .reason = Reason::Vendor, .transfers = transfers});
    ASSERT_FALSE(posted.has_value());
    EXPECT_EQ(posted.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(posted.error().detail(), alice().value);
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 70);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_entries WHERE reason = 5"), 0);
    // Chưa từng có số dư thì cũng là không đủ.
    const std::array gems{Transfer{alice(), sink(), kGem, 1}};
    const Result<Posted> none =
        post_committed({.key = key("spend-gem"), .reason = Reason::Vendor, .transfers = gems});
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::FailedPrecondition);
    expect_clean();
}

// Bút toán hợp lệ khi số dư cuối không âm: nhận rồi chuyển đi trong cùng bút toán.
TEST_F(Ledger, OnlyTheNetChangeMustBeCovered) {
    const std::array transfers{Transfer{alice(), bob(), kGem, 10},
                               Transfer{grant(), alice(), kGem, 10}};
    ASSERT_TRUE(
        post_committed({.key = key("net"), .reason = Reason::Grant, .transfers = transfers}));
    EXPECT_EQ(balance_or_minus_one(alice(), kGem), 0);
    EXPECT_EQ(balance_or_minus_one(bob(), kGem), 10);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_transfers"), 2);
    expect_clean();
}

// post kiểm bút toán trước khi chạm DB: không dòng nào được ghi, kể cả bút toán.
TEST_F(Ledger, AMalformedEntryIsRefusedBeforeTouchingTheDatabase) {
    const std::array zero{Transfer{grant(), alice(), kGold, 0}};
    const Result<Posted> posted =
        post_committed({.key = key("zero"), .reason = Reason::Grant, .transfers = zero});
    ASSERT_FALSE(posted.has_value());
    EXPECT_EQ(posted.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_entries"), 0);
}

TEST_F(Ledger, UnknownAccountsAndExternalPairsAreRefusedBeforeWriting) {
    const std::array unknown{Transfer{grant(), AccountId{999'999}, kGold, 1}};
    const Result<Posted> missing =
        post_committed({.key = key("u"), .reason = Reason::Grant, .transfers = unknown});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(missing.error().detail(), 999'999);

    const std::array external{Transfer{grant(), sink(), kGold, 1}};
    const Result<Posted> pair =
        post_committed({.key = key("e"), .reason = Reason::Grant, .transfers = external});
    ASSERT_FALSE(pair.has_value());
    EXPECT_EQ(pair.error().code(), ErrorCode::FailedPrecondition);
    const std::array item{ItemMove{ItemId{1}, kSword, grant(), sink()}};
    const Result<Posted> item_pair =
        post_committed({.key = key("ei"), .reason = Reason::Loot, .items = item});
    ASSERT_FALSE(item_pair.has_value());
    EXPECT_EQ(item_pair.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_entries"), 0);
}

TEST_F(Ledger, OnlyHoldersHaveBalances) {
    const Result<i64> external = balance_of(grant(), kGold);
    ASSERT_FALSE(external.has_value());
    EXPECT_EQ(external.error().code(), ErrorCode::FailedPrecondition);
    const Result<i64> unknown = balance_of(AccountId{424'242}, kGold);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code(), ErrorCode::NotFound);
}

TEST_F(Ledger, ReplayingAnEntryAppliesItOnce) {
    const std::array transfers{Transfer{grant(), alice(), kGold, 25}};
    const Entry entry{.key = key("once"), .reason = Reason::Purchase, .transfers = transfers};
    const Result<Posted> first = post_committed(entry);
    ASSERT_TRUE(first.has_value()) << why(first.error());
    for (int retry = 0; retry < 3; ++retry) {
        const Result<Posted> again = post_committed(entry);
        ASSERT_TRUE(again.has_value()) << why(again.error());
        EXPECT_TRUE(again->replayed);
        EXPECT_EQ(again->entry, first->entry);
    }
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 25);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_entries"), 1);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_transfers"), 1);
}

TEST_F(Ledger, AKeyCannotBeReusedForOtherContent) {
    const std::array transfers{Transfer{grant(), alice(), kGold, 25}};
    const Result<Posted> first =
        post_committed({.key = key("k"), .reason = Reason::Purchase, .transfers = transfers});
    ASSERT_TRUE(first.has_value()) << why(first.error());
    const std::array more{Transfer{grant(), alice(), kGold, 26}};
    const Result<Posted> other =
        post_committed({.key = key("k"), .reason = Reason::Purchase, .transfers = more});
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(other.error().detail(), first->entry.value);
    const Result<Posted> reason =
        post_committed({.key = key("k"), .reason = Reason::Grant, .transfers = transfers});
    ASSERT_FALSE(reason.has_value());
    EXPECT_EQ(reason.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 25);
}

// Cùng khoá hai lần trong một transaction: lần sau là replay, không áp hai lần.
TEST_F(Ledger, TheSameKeyTwiceInOneTransactionIsAReplay) {
    const std::array transfers{Transfer{grant(), alice(), kGold, 5}};
    const Entry entry{.key = key("twice"), .reason = Reason::Grant, .transfers = transfers};
    const Result<Posted> second = in_transaction(connection(), [&entry](db::Transaction& tx) {
        const Result<Posted> first = post(tx, entry);
        return first ? post(tx, entry) : first;
    });
    ASSERT_TRUE(second.has_value()) << why(second.error());
    EXPECT_TRUE(second->replayed);
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 5);
}

TEST_F(Ledger, ItemsAreCreatedMovedAndDestroyed) {
    create(ItemId{7}, kSword, alice());
    const ItemState created = state_of(ItemId{7});
    EXPECT_EQ(created.owner, alice());
    EXPECT_EQ(created.asset, kSword);
    EXPECT_EQ(created.version, 1);

    const std::array give_b{ItemMove{ItemId{7}, kSword, alice(), bob()}};
    ASSERT_TRUE(post_committed({.key = key("m1"), .reason = Reason::Trade, .items = give_b}));
    const std::array destroy{ItemMove{ItemId{7}, kSword, bob(), sink()}};
    ASSERT_TRUE(post_committed({.key = key("m2"), .reason = Reason::Consume, .items = destroy}));
    const ItemState destroyed = state_of(ItemId{7});
    EXPECT_EQ(destroyed.owner, sink());
    EXPECT_EQ(destroyed.version, 3);
    EXPECT_EQ(count("SELECT count(*) FROM ledger_item_moves WHERE item_id = 7"), 3);

    const Result<std::optional<ItemState>> never = item_of(ItemId{8});
    ASSERT_TRUE(never.has_value());
    EXPECT_FALSE(never->has_value());
    expect_clean();
}

// Vật phẩm đã huỷ không sống lại, và một id không bao giờ được tạo hai lần.
TEST_F(Ledger, ItemIdsAreNeverIssuedTwice) {
    create(ItemId{7}, kSword, alice());
    const std::array again{ItemMove{ItemId{7}, kSword, grant(), bob()}};
    const Result<Posted> duplicate =
        post_committed({.key = key("again"), .reason = Reason::Loot, .items = again});
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(duplicate.error().detail(), 7);

    const std::array destroy{ItemMove{ItemId{7}, kSword, alice(), sink()}};
    ASSERT_TRUE(post_committed({.key = key("d"), .reason = Reason::Consume, .items = destroy}));
    const std::array revive{ItemMove{ItemId{7}, kSword, sink(), alice()}};
    const Result<Posted> revived =
        post_committed({.key = key("r"), .reason = Reason::Grant, .items = revive});
    ASSERT_FALSE(revived.has_value());
    EXPECT_EQ(revived.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(owner_of(ItemId{7}), sink());
    expect_clean();
}

TEST_F(Ledger, AnItemMovesOnlyFromItsOwnerAndDefinition) {
    create(ItemId{7}, kSword, alice());
    const std::array wrong_owner{ItemMove{ItemId{7}, kSword, bob(), alice()}};
    const Result<Posted> stolen =
        post_committed({.key = key("w1"), .reason = Reason::Trade, .items = wrong_owner});
    ASSERT_FALSE(stolen.has_value());
    EXPECT_EQ(stolen.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(stolen.error().context(), "ledger: vật phẩm không ở tài khoản nguồn");

    const std::array wrong_asset{ItemMove{ItemId{7}, kShield, alice(), bob()}};
    const Result<Posted> mismatch =
        post_committed({.key = key("w2"), .reason = Reason::Trade, .items = wrong_asset});
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(mismatch.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(mismatch.error().context(), "ledger: vật phẩm khác định nghĩa");

    const std::array missing{ItemMove{ItemId{8}, kSword, alice(), bob()}};
    const Result<Posted> unknown =
        post_committed({.key = key("w3"), .reason = Reason::Trade, .items = missing});
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(unknown.error().detail(), 8);
    EXPECT_EQ(owner_of(ItemId{7}), alice());
    expect_clean();
}

// Trao đổi: tiền và vật phẩm hai chiều trong một bút toán; một vế hỏng thì không vế nào áp.
TEST_F(Ledger, ATradeIsAllOrNothing) {
    give(alice(), kGold, 50);
    create(ItemId{1}, kSword, alice());
    create(ItemId{2}, kShield, bob());
    const std::array pay{Transfer{alice(), bob(), kGold, 50}};
    const std::array swap{ItemMove{ItemId{1}, kSword, alice(), bob()},
                          ItemMove{ItemId{2}, kShield, bob(), alice()}};
    ASSERT_TRUE(post_committed(
        {.key = key("trade"), .reason = Reason::Trade, .transfers = pay, .items = swap}));
    EXPECT_EQ(balance_or_minus_one(alice(), kGold), 0);
    EXPECT_EQ(balance_or_minus_one(bob(), kGold), 50);
    EXPECT_EQ(owner_of(ItemId{1}), bob());
    EXPECT_EQ(owner_of(ItemId{2}), alice());

    // B trả lại kiếm và đòi 60 vàng mà A không có: kiếm đi trước trong thứ tự áp, rồi bị rollback.
    const std::array overpay{Transfer{alice(), bob(), kGold, 60}};
    const std::array back{ItemMove{ItemId{1}, kSword, bob(), alice()}};
    const Result<Posted> refused = post_committed(
        {.key = key("trade-2"), .reason = Reason::Trade, .transfers = overpay, .items = back});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(owner_of(ItemId{1}), bob());
    EXPECT_EQ(balance_or_minus_one(bob(), kGold), 50);
    expect_clean();
}

// Hơn 32 tài khoản trong một bút toán: tra loại cần nhiều câu.
TEST_F(Ledger, EntriesWithManyAccountsAreLookedUpInBatches) {
    std::vector<AccountId> holders;
    for (usize i = 0; i < kMaxTransfers + kMaxItemMoves; ++i) {
        const Result<AccountId> holder = new_holder();
        ASSERT_TRUE(holder.has_value());
        holders.push_back(*holder);
    }
    std::vector<Transfer> transfers;
    std::vector<ItemMove> items;
    for (usize i = 0; i < kMaxTransfers; ++i) {
        transfers.push_back(Transfer{grant(), holders[i], kGem, narrow<i64>(i + 1)});
        items.push_back(
            ItemMove{ItemId{narrow<i64>(1'000 + i)}, kSword, grant(), holders[kMaxTransfers + i]});
    }
    ASSERT_TRUE(post_committed(
        {.key = key("wide"), .reason = Reason::Loot, .transfers = transfers, .items = items}));
    EXPECT_EQ(balance_or_minus_one(holders[0], kGem), 1);
    EXPECT_EQ(balance_or_minus_one(holders[kMaxTransfers - 1], kGem), 32);
    EXPECT_EQ(owner_of(ItemId{1'000 + kMaxItemMoves - 1}), holders.back());
    expect_clean();
}

TEST_F(Ledger, NamedAccountsAreFoundOrCreated) {
    const Result<AccountId> first = account_named(AccountKind::External, "store.revenue");
    const Result<AccountId> second = account_named(AccountKind::External, "store.revenue");
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(*first, *second);
    const Result<AccountId> other_kind = account_named(AccountKind::Holder, "store.revenue");
    ASSERT_FALSE(other_kind.has_value());
    EXPECT_EQ(other_kind.error().code(), ErrorCode::FailedPrecondition);
    const Result<AccountId> escrow = account_named(AccountKind::Holder, "auction.escrow_1");
    ASSERT_TRUE(escrow.has_value());
    EXPECT_NE(*escrow, *first);
    const std::string longest = "a" + std::string(kMaxCodeBytes - 1, 'b');
    EXPECT_TRUE(account_named(AccountKind::External, longest).has_value());
    const std::string too_long = longest + "c";
    for (const std::string_view bad :
         {std::string_view{}, std::string_view{"Store"}, std::string_view{"1abc"},
          std::string_view{"a b"}, std::string_view{"a-b"}, std::string_view{"_a"},
          std::string_view{too_long}}) {
        const Result<AccountId> refused = account_named(AccountKind::External, bad);
        ASSERT_FALSE(refused.has_value()) << bad;
        EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument) << bad;
    }
}

// Số dư gần giới hạn của int8 (dựng bằng tay, ngoài ledger): cộng tràn là OutOfRange, không bao
// giờ quay vòng. Soát sổ thấy dòng số dư sửa tay.
TEST_F(Ledger, BalanceOverflowIsRefused) {
    give(alice(), kGold, 1);
    ASSERT_TRUE(connection()
                    .execute_script(std::format("UPDATE ledger_balances SET balance = {} "
                                                "WHERE account_id = {}",
                                                std::numeric_limits<i64>::max() - 5, alice().value),
                                    deadline())
                    .has_value());
    const std::array transfers{Transfer{grant(), alice(), kGold, 10}};
    const Result<Posted> overflow =
        post_committed({.key = key("big"), .reason = Reason::Grant, .transfers = transfers});
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::OutOfRange);
    const Result<Audit> report = audit_now();
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->balance_mismatches, 1U);
}

// Soát sổ bắt từng kiểu sửa tay vào bảng: số dư, chủ vật phẩm, chuỗi lượt chuyển.
TEST_F(Ledger, AuditFindsEveryKindOfTampering) {
    give(alice(), kGold, 40);
    create(ItemId{1}, kSword, alice());
    create(ItemId{2}, kSword, bob());
    expect_clean();
    const auto tamper = [this](const std::string& sql) {
        ASSERT_TRUE(connection().execute_script(sql, deadline()).has_value()) << sql;
    };
    tamper(std::format("UPDATE ledger_balances SET balance = 41 WHERE account_id = {}",
                       alice().value));
    tamper(std::format("INSERT INTO ledger_balances (account_id, asset, balance) VALUES ({}, 2, 3)",
                       bob().value));
    tamper(std::format("UPDATE ledger_items SET account_id = {} WHERE id = 1", bob().value));
    // Lượt thứ hai của vật phẩm 2 đi từ A tới B, trong khi lượt đầu đã đưa nó tới B: chuỗi đứt, dù
    // chủ hiện tại khớp lượt cuối.
    tamper(
        std::format("UPDATE ledger_items SET version = 2 WHERE id = 2; "
                    "INSERT INTO ledger_item_moves VALUES "
                    "((SELECT max(id) FROM ledger_entries), 1, 2, 2, {}, {})",
                    alice().value, bob().value));
    const Result<Audit> report = audit_now();
    ASSERT_TRUE(report.has_value()) << why(report.error());
    EXPECT_EQ(report->balance_mismatches, 2U);
    EXPECT_EQ(report->owner_mismatches, 1U);
    EXPECT_EQ(report->broken_chains, 1U);
    EXPECT_FALSE(report->clean());
}

}  // namespace
}  // namespace orion::ledger
