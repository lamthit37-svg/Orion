// Connect token (docs/formats/connect_token.md). `craft` ghi byte theo đặc tả, độc lập với
// issue_connect_token, rồi ký bằng engine/crypto; mỗi luật của bảng "Lỗi" được kiểm bằng cách sửa
// đúng một trường rồi ký lại, để lỗi đo được là của luật đó chứ không phải của chữ ký.

#include "engine/net/connect_token.hpp"

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace orion::net {
namespace {

using core::Duration;
using core::WallTime;

constexpr i64 kI64Min = std::numeric_limits<i64>::min();
constexpr i64 kI64Max = std::numeric_limits<i64>::max();

[[nodiscard]] crypto::SigningKeyPair signing_key(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{fill});
    return crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
}

[[nodiscard]] crypto::KeyExchangePublicKey client_key(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kKeyExchangeSecretKeySize> secret{};
    secret.fill(std::byte{fill});
    return crypto::key_exchange_key_pair_from_secret(crypto::KeyExchangeSecretKey(secret))
        .public_key;
}

// Các trường của một token, ghi thẳng theo bảng của đặc tả (thời điểm là micro giây).
struct Fields {
    std::array<std::byte, 8> magic = kConnectTokenMagic;
    u16 version = 1;
    u16 flags = 0;
    u64 account_id = 42;
    i64 issued_at = 1'790'000'000'000'000;
    i64 expires_at = 1'790'000'060'000'000;
    crypto::KeyExchangePublicKey client;
    crypto::SigningPublicKey server;
};

[[nodiscard]] std::vector<std::byte> craft(const Fields& fields,
                                           const crypto::SigningKeyPair& signer) {
    std::vector<std::byte> out(196);
    const std::span<std::byte> bytes(out);
    std::ranges::copy(fields.magic, bytes.begin());
    core::store_le<u16>(bytes.subspan<8, 2>(), fields.version);
    core::store_le<u16>(bytes.subspan<10, 2>(), fields.flags);
    core::store_le<u64>(bytes.subspan<12, 8>(), fields.account_id);
    core::store_le<i64>(bytes.subspan<20, 8>(), fields.issued_at);
    core::store_le<i64>(bytes.subspan<28, 8>(), fields.expires_at);
    std::ranges::copy(fields.client.bytes, bytes.begin() + 36);
    std::ranges::copy(fields.server.bytes, bytes.begin() + 68);
    std::ranges::copy(signer.public_key.bytes, bytes.begin() + 100);
    const crypto::Signature signature = crypto::sign(bytes.first(132), signer.secret_key);
    std::ranges::copy(signature.bytes, bytes.begin() + 132);
    return out;
}

void expect_same_claims(const ConnectTokenClaims& a, const ConnectTokenClaims& b) {
    EXPECT_EQ(a.account_id, b.account_id);
    EXPECT_EQ(a.issued_at, b.issued_at);
    EXPECT_EQ(a.expires_at, b.expires_at);
    EXPECT_TRUE(crypto::equal_constant_time(a.client_key, b.client_key));
    EXPECT_TRUE(crypto::equal_constant_time(a.server_key, b.server_key));
}

class ConnectTokenTest : public ::testing::Test {
protected:
    [[nodiscard]] Fields fields() const {
        Fields f;
        f.client = client_;
        f.server = server_.public_key;
        return f;
    }

    [[nodiscard]] ConnectTokenClaims claims() const {
        const Fields f = fields();
        ConnectTokenClaims c;
        c.account_id = f.account_id;
        c.issued_at = WallTime::from_unix_microseconds(f.issued_at);
        c.expires_at = WallTime::from_unix_microseconds(f.expires_at);
        c.client_key = f.client;
        c.server_key = f.server;
        return c;
    }

    // Một giây sau lúc ký: token còn hạn.
    [[nodiscard]] static WallTime now() {
        return WallTime::from_unix_microseconds(Fields{}.issued_at + 1'000'000);
    }

    [[nodiscard]] Result<VerifiedConnectToken> verify(const std::span<const std::byte> token,
                                                      const WallTime at = now()) const {
        const std::array trusted = {auth_.public_key};
        return VerifiedConnectToken::verify(token, trusted, server_.public_key, at);
    }

    [[nodiscard]] ErrorCode verify_error(const std::span<const std::byte> token,
                                         const WallTime at = now()) const {
        const Result<VerifiedConnectToken> result = verify(token, at);
        EXPECT_FALSE(result.has_value());
        return result.has_value() ? ErrorCode::Internal : result.error().code();
    }

    [[nodiscard]] const crypto::SigningKeyPair& auth() const noexcept { return auth_; }
    [[nodiscard]] const crypto::SigningKeyPair& other_auth() const noexcept { return other_auth_; }
    [[nodiscard]] const crypto::SigningKeyPair& server() const noexcept { return server_; }
    [[nodiscard]] const crypto::SigningKeyPair& other_server() const noexcept {
        return other_server_;
    }

private:
    crypto::SigningKeyPair auth_ = signing_key(0x11);
    crypto::SigningKeyPair other_auth_ = signing_key(0x22);
    crypto::SigningKeyPair server_ = signing_key(0x33);
    crypto::SigningKeyPair other_server_ = signing_key(0x44);
    crypto::KeyExchangePublicKey client_ = client_key(0x55);
};

TEST_F(ConnectTokenTest, IssueThenVerifyKeepsEveryClaim) {
    const Result<ConnectToken> token = issue_connect_token(claims(), auth());
    ASSERT_TRUE(token.has_value());
    const Result<VerifiedConnectToken> verified = verify(token->view());
    ASSERT_TRUE(verified.has_value());
    expect_same_claims(verified->claims(), claims());
    EXPECT_TRUE(crypto::equal_constant_time(verified->signer(), auth().public_key));
}

TEST_F(ConnectTokenTest, IssueWritesExactlyTheSpecifiedBytes) {
    const Result<ConnectToken> token = issue_connect_token(claims(), auth());
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(std::ranges::equal(token->bytes, craft(fields(), auth())));
    // Ed25519 tất định: ký lại cho đúng các byte đó.
    const Result<ConnectToken> again = issue_connect_token(claims(), auth());
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(std::ranges::equal(token->bytes, again->bytes));
}

TEST_F(ConnectTokenTest, IssueRejectsClaimsOutsideTheRules) {
    ConnectTokenClaims zero_account = claims();
    zero_account.account_id = 0;
    ConnectTokenClaims backwards = claims();
    backwards.expires_at = backwards.issued_at;
    ConnectTokenClaims too_long = claims();
    too_long.expires_at = too_long.issued_at + kMaxConnectTokenLifetime + Duration::microseconds(1);
    for (const ConnectTokenClaims& bad : {zero_account, backwards, too_long}) {
        const Result<ConnectToken> token = issue_connect_token(bad, auth());
        ASSERT_FALSE(token.has_value());
        EXPECT_EQ(token.error().code(), ErrorCode::InvalidArgument);
    }
    ConnectTokenClaims longest = claims();
    longest.expires_at = longest.issued_at + kMaxConnectTokenLifetime;
    EXPECT_TRUE(issue_connect_token(longest, auth()).has_value());
}

TEST_F(ConnectTokenTest, InspectReadsClaimsWithoutCheckingTheSignature) {
    std::vector<std::byte> token = craft(fields(), auth());
    std::ranges::fill(std::span(token).subspan(132), std::byte{0});
    const Result<ConnectTokenClaims> seen = inspect_connect_token(token);
    ASSERT_TRUE(seen.has_value());
    expect_same_claims(*seen, claims());
    // Còn gateway thì từ chối đúng token đó.
    EXPECT_EQ(verify_error(token), ErrorCode::DataLoss);
}

TEST_F(ConnectTokenTest, InspectAppliesStructureAndClaimRules) {
    Fields zero_account = fields();
    zero_account.account_id = 0;
    const Result<ConnectTokenClaims> bad_claims =
        inspect_connect_token(craft(zero_account, auth()));
    ASSERT_FALSE(bad_claims.has_value());
    EXPECT_EQ(bad_claims.error().code(), ErrorCode::DataLoss);
    Fields version = fields();
    version.version = 2;
    const Result<ConnectTokenClaims> bad_version = inspect_connect_token(craft(version, auth()));
    ASSERT_FALSE(bad_version.has_value());
    EXPECT_EQ(bad_version.error().code(), ErrorCode::Unimplemented);
    const std::vector<std::byte> shorter(195);
    const Result<ConnectTokenClaims> bad_size = inspect_connect_token(shorter);
    ASSERT_FALSE(bad_size.has_value());
    EXPECT_EQ(bad_size.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ConnectTokenTest, StructureRules) {
    std::vector<std::byte> longer = craft(fields(), auth());
    longer.push_back(std::byte{0});
    EXPECT_EQ(verify_error(longer), ErrorCode::InvalidArgument);
    EXPECT_EQ(verify_error(std::span(longer).first(195)), ErrorCode::InvalidArgument);
    EXPECT_EQ(verify_error({}), ErrorCode::InvalidArgument);
    Fields magic = fields();
    magic.magic[7] = std::byte{'X'};
    EXPECT_EQ(verify_error(craft(magic, auth())), ErrorCode::InvalidArgument);
    Fields version = fields();
    version.version = 0;
    EXPECT_EQ(verify_error(craft(version, auth())), ErrorCode::Unimplemented);
    Fields flags = fields();
    flags.flags = 0x8000;
    EXPECT_EQ(verify_error(craft(flags, auth())), ErrorCode::DataLoss);
}

TEST_F(ConnectTokenTest, OnlyTrustedSignersAreAccepted) {
    EXPECT_EQ(verify_error(craft(fields(), other_auth())), ErrorCode::Unauthenticated);
    // Xoay khoá: danh sách có cả khoá cũ và khoá mới thì token của cả hai đều được nhận.
    const std::array both = {other_auth().public_key, auth().public_key};
    for (const crypto::SigningKeyPair* signer : {&auth(), &other_auth()}) {
        EXPECT_TRUE(
            VerifiedConnectToken::verify(craft(fields(), *signer), both, server().public_key, now())
                .has_value());
    }
    const Result<VerifiedConnectToken> nobody =
        VerifiedConnectToken::verify(craft(fields(), auth()), {}, server().public_key, now());
    ASSERT_FALSE(nobody.has_value());
    EXPECT_EQ(nobody.error().code(), ErrorCode::Unauthenticated);
}

// Lật một bit ở từng byte: lỗi theo vùng của byte đó và theo thứ tự kiểm.
TEST_F(ConnectTokenTest, EveryByteIsProtected) {
    const std::vector<std::byte> original = craft(fields(), auth());
    for (usize i = 0; i < original.size(); ++i) {
        for (const std::byte bit : {std::byte{0x01}, std::byte{0x80}}) {
            std::vector<std::byte> token = original;
            token[i] ^= bit;
            ErrorCode expected = ErrorCode::DataLoss;  // flags, các trường đã ký, chữ ký
            if (i < 8) {
                expected = ErrorCode::InvalidArgument;
            } else if (i < 10) {
                expected = ErrorCode::Unimplemented;
            } else if (i >= 100 && i < 132) {
                expected = ErrorCode::Unauthenticated;
            }
            EXPECT_EQ(verify_error(token), expected) << "byte " << i;
        }
    }
}

// Mỗi luật ở bước 4 kiểm trên token có chữ ký đúng của khoá tin cậy.
TEST_F(ConnectTokenTest, SignedClaimsOutsideTheRulesAreDataLoss) {
    Fields zero_account = fields();
    zero_account.account_id = 0;
    Fields backwards = fields();
    backwards.expires_at = backwards.issued_at - 1;
    Fields equal = fields();
    equal.expires_at = equal.issued_at;
    Fields too_long = fields();
    too_long.expires_at = too_long.issued_at + kMaxConnectTokenLifetime.as_microseconds() + 1;
    // Hiệu của hai thời điểm vượt i64: phải được tính không tràn.
    Fields huge = fields();
    huge.issued_at = kI64Min;
    huge.expires_at = kI64Max;
    for (const Fields& bad : {zero_account, backwards, equal, too_long, huge}) {
        EXPECT_EQ(verify_error(craft(bad, auth())), ErrorCode::DataLoss);
    }
    Fields longest = fields();
    longest.expires_at = longest.issued_at + kMaxConnectTokenLifetime.as_microseconds();
    EXPECT_TRUE(verify(craft(longest, auth())).has_value());
}

TEST_F(ConnectTokenTest, TokenForAnotherServerIsPermissionDenied) {
    Fields other = fields();
    other.server = other_server().public_key;
    EXPECT_EQ(verify_error(craft(other, auth())), ErrorCode::PermissionDenied);
}

TEST_F(ConnectTokenTest, ExpiryIsExclusive) {
    const std::vector<std::byte> token = craft(fields(), auth());
    const i64 expires = fields().expires_at;
    EXPECT_TRUE(verify(token, WallTime::from_unix_microseconds(expires - 1)).has_value());
    EXPECT_EQ(verify_error(token, WallTime::from_unix_microseconds(expires)),
              ErrorCode::DeadlineExceeded);
    EXPECT_EQ(verify_error(token, WallTime::from_unix_microseconds(kI64Max)),
              ErrorCode::DeadlineExceeded);
}

TEST_F(ConnectTokenTest, IssuedInTheFutureIsToleratedUpToTheClockSkew) {
    const std::vector<std::byte> token = craft(fields(), auth());
    const i64 issued = fields().issued_at;
    const i64 skew = kConnectTokenClockSkew.as_microseconds();
    EXPECT_TRUE(verify(token, WallTime::from_unix_microseconds(issued - skew)).has_value());
    EXPECT_EQ(verify_error(token, WallTime::from_unix_microseconds(issued - skew - 1)),
              ErrorCode::FailedPrecondition);
    EXPECT_EQ(verify_error(token, WallTime::from_unix_microseconds(kI64Min)),
              ErrorCode::FailedPrecondition);
}

// Thời điểm ở hai đầu của i64: mọi phép so sánh phải không tràn (UBSan bắt được nếu có).
TEST_F(ConnectTokenTest, ExtremeTimesDoNotOverflow) {
    Fields earliest = fields();
    earliest.issued_at = kI64Min;
    earliest.expires_at = kI64Min + 1;
    EXPECT_EQ(verify_error(craft(earliest, auth()), WallTime::from_unix_microseconds(0)),
              ErrorCode::DeadlineExceeded);
    Fields latest = fields();
    latest.issued_at = kI64Max - 1;
    latest.expires_at = kI64Max;
    EXPECT_EQ(verify_error(craft(latest, auth()), WallTime::from_unix_microseconds(kI64Min)),
              ErrorCode::FailedPrecondition);
    EXPECT_TRUE(
        verify(craft(latest, auth()), WallTime::from_unix_microseconds(kI64Max - 1)).has_value());
}

}  // namespace
}  // namespace orion::net
