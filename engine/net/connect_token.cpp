#include "engine/net/connect_token.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>

namespace orion::net {
namespace {

// Vị trí các trường (docs/formats/connect_token.md, mục Bố cục).
constexpr usize kVersionOffset = 8;
constexpr usize kFlagsOffset = 10;
constexpr usize kAccountIdOffset = 12;
constexpr usize kIssuedAtOffset = 20;
constexpr usize kExpiresAtOffset = 28;
constexpr usize kClientKeyOffset = 36;
constexpr usize kServerKeyOffset = 68;
constexpr usize kSignerOffset = 100;
constexpr usize kSignedSize = 132;
static_assert(kSignedSize + crypto::kSignatureSize == kConnectTokenSize);

using TokenView = std::span<const std::byte, kConnectTokenSize>;

// Hiệu b − a của hai số i64 với a < b, tính trong u64 để không tràn: hiệu thật luôn vừa u64.
[[nodiscard]] constexpr u64 distance(const i64 a, const i64 b) noexcept {
    return static_cast<u64>(b) - static_cast<u64>(a);
}

// Luật ở bước 4 của mục Kiểm. Các trường có thể là bất kỳ giá trị i64 nào, nên không dùng phép trừ
// của WallTime (tràn số có dấu là UB).
[[nodiscard]] bool claims_are_valid(const ConnectTokenClaims& claims) noexcept {
    const i64 issued = claims.issued_at.unix_microseconds();
    const i64 expires = claims.expires_at.unix_microseconds();
    constexpr auto kMaxLifetime = static_cast<u64>(kMaxConnectTokenLifetime.as_microseconds());
    return claims.account_id != 0 && expires > issued && distance(issued, expires) <= kMaxLifetime;
}

// Bước 1: cỡ, magic, version, flags.
[[nodiscard]] Result<TokenView> check_structure(const std::span<const std::byte> token) noexcept {
    if (token.size() != kConnectTokenSize ||
        !std::ranges::equal(token.first<kConnectTokenMagic.size()>(), kConnectTokenMagic)) {
        return fail(ErrorCode::InvalidArgument, "connect token: không phải connect token",
                    static_cast<i64>(token.size()));
    }
    const TokenView view = token.first<kConnectTokenSize>();
    const auto version = core::load_le<u16>(view.subspan<kVersionOffset, sizeof(u16)>());
    if (version != kConnectTokenVersion) {
        return fail(ErrorCode::Unimplemented, "connect token: phiên bản định dạng lạ", version);
    }
    const auto flags = core::load_le<u16>(view.subspan<kFlagsOffset, sizeof(u16)>());
    if (flags != 0) {
        return fail(ErrorCode::DataLoss, "connect token: flags khác 0", flags);
    }
    return view;
}

// Bước 2 và 3: người ký nằm trong `trusted` (duyệt hết danh sách, không dừng sớm) và chữ ký đúng
// trên 132 byte đầu. Ghi người ký vào `signer`.
[[nodiscard]] Result<void> authenticate(const TokenView token,
                                        const std::span<const crypto::SigningPublicKey> trusted,
                                        crypto::SigningPublicKey& signer) noexcept {
    std::ranges::copy(token.subspan<kSignerOffset, crypto::kSigningPublicKeySize>(),
                      signer.bytes.begin());
    bool known = false;
    for (const crypto::SigningPublicKey& key : trusted) {
        known = crypto::equal_constant_time(key, signer) || known;
    }
    if (!known) {
        return fail(ErrorCode::Unauthenticated, "connect token: người ký không được tin");
    }
    crypto::Signature signature;
    std::ranges::copy(token.subspan<kSignedSize, crypto::kSignatureSize>(),
                      signature.bytes.begin());
    if (!crypto::verify(token.first<kSignedSize>(), signature, signer)) {
        return fail(ErrorCode::DataLoss, "connect token: chữ ký sai");
    }
    return {};
}

[[nodiscard]] ConnectTokenClaims read_claims(const TokenView token) noexcept {
    ConnectTokenClaims claims;
    claims.account_id = core::load_le<u64>(token.subspan<kAccountIdOffset, sizeof(u64)>());
    claims.issued_at = core::WallTime::from_unix_microseconds(
        core::load_le<i64>(token.subspan<kIssuedAtOffset, sizeof(i64)>()));
    claims.expires_at = core::WallTime::from_unix_microseconds(
        core::load_le<i64>(token.subspan<kExpiresAtOffset, sizeof(i64)>()));
    std::ranges::copy(token.subspan<kClientKeyOffset, crypto::kKeyExchangePublicKeySize>(),
                      claims.client_key.bytes.begin());
    std::ranges::copy(token.subspan<kServerKeyOffset, crypto::kSigningPublicKeySize>(),
                      claims.server_key.bytes.begin());
    return claims;
}

// Bước 6. `now` và các trường là i64 bất kỳ; mọi phép trừ đi qua distance.
[[nodiscard]] Result<void> check_time(const ConnectTokenClaims& claims,
                                      const core::WallTime now) noexcept {
    const i64 issued = claims.issued_at.unix_microseconds();
    const i64 current = now.unix_microseconds();
    constexpr auto kSkew = static_cast<u64>(kConnectTokenClockSkew.as_microseconds());
    if (issued > current && distance(current, issued) > kSkew) {
        return fail(ErrorCode::FailedPrecondition, "connect token: ký ở tương lai, lệch đồng hồ");
    }
    if (current >= claims.expires_at.unix_microseconds()) {
        return fail(ErrorCode::DeadlineExceeded, "connect token: hết hạn");
    }
    return {};
}

}  // namespace

Result<ConnectToken> issue_connect_token(const ConnectTokenClaims& claims,
                                         const crypto::SigningKeyPair& signer) noexcept {
    if (!claims_are_valid(claims)) {
        return fail(ErrorCode::InvalidArgument, "connect token: các trường sai luật");
    }
    ConnectToken token;
    const std::span<std::byte, kSignedSize> body = std::span(token.bytes).first<kSignedSize>();
    core::ByteWriter writer(body);
    writer.write_bytes(kConnectTokenMagic);
    writer.write<u16>(kConnectTokenVersion);
    writer.write<u16>(0);
    writer.write<u64>(claims.account_id);
    writer.write<i64>(claims.issued_at.unix_microseconds());
    writer.write<i64>(claims.expires_at.unix_microseconds());
    writer.write_bytes(claims.client_key.view());
    writer.write_bytes(claims.server_key.view());
    writer.write_bytes(signer.public_key.view());
    ORION_VERIFY(!writer.overflowed() && writer.offset() == kSignedSize,
                 "connect token: tính sai cỡ");
    const crypto::Signature signature = crypto::sign(body, signer.secret_key);
    std::ranges::copy(signature.bytes, std::span(token.bytes).subspan<kSignedSize>().begin());
    return token;
}

Result<ConnectTokenClaims> inspect_connect_token(const std::span<const std::byte> token) noexcept {
    const Result<TokenView> view = check_structure(token);
    if (!view) {
        return std::unexpected(view.error());
    }
    const ConnectTokenClaims claims = read_claims(*view);
    if (!claims_are_valid(claims)) {
        return fail(ErrorCode::DataLoss, "connect token: các trường sai luật");
    }
    return claims;
}

Result<VerifiedConnectToken> VerifiedConnectToken::verify(
    const std::span<const std::byte> token,
    const std::span<const crypto::SigningPublicKey> trusted_signers,
    const crypto::SigningPublicKey& server_key, const core::WallTime now) noexcept {
    const Result<TokenView> view = check_structure(token);
    if (!view) {
        return std::unexpected(view.error());
    }
    VerifiedConnectToken result;
    if (const Result<void> authentic = authenticate(*view, trusted_signers, result.signer_);
        !authentic) {
        return std::unexpected(authentic.error());
    }

    // Từ đây dữ liệu đã được ký bởi một khoá tin cậy.
    result.claims_ = read_claims(*view);
    if (!claims_are_valid(result.claims_)) {
        return fail(ErrorCode::DataLoss, "connect token: các trường sai luật");
    }
    if (!crypto::equal_constant_time(result.claims_.server_key, server_key)) {
        return fail(ErrorCode::PermissionDenied, "connect token: dành cho cụm server khác");
    }
    if (const Result<void> timely = check_time(result.claims_, now); !timely) {
        return std::unexpected(timely.error());
    }
    return result;
}

}  // namespace orion::net
