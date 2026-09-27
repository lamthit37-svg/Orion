#pragma once

// Connect token (docs/formats/connect_token.md): auth ký, gateway kiểm tại chỗ, client mang theo
// vào bắt tay (CLAUDE.md X.9). Đây là chỗ duy nhất tạo và kiểm token; auth và gateway cùng gọi code
// này (X.14).
//
// - issue_connect_token: auth ký các trường của một token.
// - inspect_connect_token: client đọc các trường mà không kiểm chữ ký.
// - VerifiedConnectToken::verify: gateway kiểm đủ. Kiểu này chỉ dựng được qua verify, nên có nó
//   trong tay là token đã qua mọi bước kiểm.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace orion::net {

inline constexpr std::array<std::byte, 8> kConnectTokenMagic = {
    std::byte{'O'}, std::byte{'R'}, std::byte{'I'}, std::byte{'O'},
    std::byte{'N'}, std::byte{'T'}, std::byte{'O'}, std::byte{'K'}};
inline constexpr u16 kConnectTokenVersion = 1;
inline constexpr usize kConnectTokenSize = 196;
// Chính sách, không phải số đo (mục "Chính sách" của định dạng).
inline constexpr core::Duration kMaxConnectTokenLifetime = core::Duration::seconds(120);
inline constexpr core::Duration kConnectTokenClockSkew = core::Duration::seconds(10);

struct ConnectTokenTag;
using ConnectToken = crypto::PublicBytes<kConnectTokenSize, ConnectTokenTag>;

// Các trường auth ký vào token.
struct ConnectTokenClaims {
    // Id tài khoản (ADR 0006), khác 0.
    u64 account_id = 0;
    core::WallTime issued_at;
    // Sau issued_at, không quá kMaxConnectTokenLifetime sau nó.
    core::WallTime expires_at;
    // Khoá công khai X25519 client dùng để bắt tay.
    crypto::KeyExchangePublicKey client_key;
    // Khoá định danh Ed25519 của cụm server mà token dành cho; client kiểm chữ ký của server trong
    // bắt tay bằng khoá này.
    crypto::SigningPublicKey server_key;
};

// Ký `claims` bằng khoá của auth. Cùng đầu vào luôn cho cùng token (Ed25519 tất định). Lỗi:
// InvalidArgument khi account_id là 0, expires_at không sau issued_at, hay tuổi thọ vượt
// kMaxConnectTokenLifetime.
[[nodiscard]] Result<ConnectToken> issue_connect_token(
    const ConnectTokenClaims& claims, const crypto::SigningKeyPair& signer) noexcept;

// Đọc các trường của token mà không kiểm chữ ký: cho client, nơi token đến từ auth qua TLS và chỉ
// cần server_key (để kiểm server trong bắt tay) với expires_at (để biết lúc xin token mới). Kiểm
// bước 1 và bước 4 của định dạng; lỗi theo bảng "Lỗi". Hàm toàn phần trên `token`.
[[nodiscard]] Result<ConnectTokenClaims> inspect_connect_token(
    std::span<const std::byte> token) noexcept;

class VerifiedConnectToken {
public:
    // Kiểm `token` theo docs/formats/connect_token.md: đúng cấu trúc, người ký nằm trong
    // `trusted_signers`, chữ ký đúng, các trường đúng luật, dành cho `server_key`, và còn hạn ở
    // thời điểm `now`. Hàm toàn phần trên `token`; lỗi theo bảng "Lỗi" của định dạng.
    [[nodiscard]] static Result<VerifiedConnectToken> verify(
        std::span<const std::byte> token, std::span<const crypto::SigningPublicKey> trusted_signers,
        const crypto::SigningPublicKey& server_key, core::WallTime now) noexcept;

    [[nodiscard]] const ConnectTokenClaims& claims() const noexcept { return claims_; }
    [[nodiscard]] const crypto::SigningPublicKey& signer() const noexcept { return signer_; }

private:
    VerifiedConnectToken() = default;

    ConnectTokenClaims claims_;
    crypto::SigningPublicKey signer_;
};

}  // namespace orion::net
