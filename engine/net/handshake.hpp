#pragma once

// Gói bắt tay của transport (docs/formats/transport.md, mục Bắt tay; CLAUDE.md X.9): đọc và ghi năm
// gói REQUEST, CHALLENGE, RESPONSE, ACCEPT, REJECT; cookie không trạng thái của server; chữ ký của
// server trên ACCEPT. Máy trạng thái của client và server dùng các khối này ở tầng trên.
//
// Mọi hàm read_* là hàm toàn phần trên dữ liệu từ mạng; lỗi theo bảng "Lỗi" của định dạng.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connect_token.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace orion::net {

inline constexpr usize kRequestSize = 1200;
inline constexpr usize kChallengeSize = 49;
inline constexpr usize kResponseSize = 249;
inline constexpr usize kAcceptSize = 117;
inline constexpr usize kRejectSize = 18;
inline constexpr usize kHandshakeNonceSize = 16;
inline constexpr usize kCookieSize = 32;
// Chính sách, không phải số đo (mục Cookie của định dạng).
inline constexpr core::Duration kCookieLifetime = core::Duration::seconds(10);
inline constexpr core::Duration kCookieKeyLifetime = core::Duration::seconds(60);

struct HandshakeNonceTag;
struct CookieTag;
using HandshakeNonce = crypto::PublicBytes<kHandshakeNonceSize, HandshakeNonceTag>;
using Cookie = crypto::PublicBytes<kCookieSize, CookieTag>;

enum class RejectReason : u8 {
    VersionMismatch = 1,
    TokenInvalid = 2,
    TokenExpired = 3,
    TokenUsed = 4,
    ServerFull = 5,
};

struct Request {
    u32 protocol_version = 0;
    HandshakeNonce nonce;
    ConnectToken token;
};

struct Challenge {
    HandshakeNonce nonce;
    Cookie cookie;
};

struct Response {
    u32 protocol_version = 0;
    HandshakeNonce nonce;
    Cookie cookie;
    ConnectToken token;
};

struct Accept {
    HandshakeNonce nonce;
    u32 connection_id = 0;
    crypto::KeyExchangePublicKey server_key_exchange;
    crypto::Signature signature;
};

struct Reject {
    HandshakeNonce nonce;
    RejectReason reason = RejectReason::TokenInvalid;
};

[[nodiscard]] std::array<std::byte, kRequestSize> write_request(const Request& request) noexcept;
[[nodiscard]] std::array<std::byte, kChallengeSize> write_challenge(
    const Challenge& challenge) noexcept;
[[nodiscard]] std::array<std::byte, kResponseSize> write_response(
    const Response& response) noexcept;
[[nodiscard]] std::array<std::byte, kAcceptSize> write_accept(const Accept& accept) noexcept;
[[nodiscard]] std::array<std::byte, kRejectSize> write_reject(const Reject& reject) noexcept;

// Chỉ kiểm cỡ, prefix và các luật của chính gói; token và cookie được kiểm ở bước sau.
[[nodiscard]] Result<Request> read_request(std::span<const std::byte> packet) noexcept;
[[nodiscard]] Result<Challenge> read_challenge(std::span<const std::byte> packet) noexcept;
[[nodiscard]] Result<Response> read_response(std::span<const std::byte> packet) noexcept;
[[nodiscard]] Result<Accept> read_accept(std::span<const std::byte> packet) noexcept;
[[nodiscard]] Result<Reject> read_reject(std::span<const std::byte> packet) noexcept;

// hash(connect_token), nối token với cookie và với chữ ký của ACCEPT.
[[nodiscard]] crypto::Hash token_hash(const ConnectToken& token) noexcept;

// Chữ ký của server trên bản ghi ACCEPT (mục ACCEPT của định dạng).
[[nodiscard]] crypto::Signature sign_accept(u32 protocol_version, const HandshakeNonce& nonce,
                                            const crypto::Hash& token_hash, u32 connection_id,
                                            const crypto::KeyExchangePublicKey& server_key_exchange,
                                            const crypto::SigningSecretKey& identity) noexcept;
// true khi `accept` được ký bằng khoá định danh `identity` cho đúng lần thử này.
[[nodiscard]] bool verify_accept(const Accept& accept, u32 protocol_version,
                                 const crypto::Hash& token_hash,
                                 const crypto::SigningPublicKey& identity) noexcept;

// Cookie không trạng thái của server (mục Cookie của định dạng): khoá ngẫu nhiên thay mới mỗi
// kCookieKeyLifetime, cookie sống kCookieLifetime. Thuộc luồng IO của server; không đồng bộ.
class CookieJar {
public:
    // Khoá đầu tiên được tạo lúc `now`. crypto::initialize phải thành công trước đó.
    explicit CookieJar(core::MonoTime now) noexcept;

    // Cấp một cookie cho lần thử có các trường này từ `from`. Thay khoá khi tới hạn.
    [[nodiscard]] Cookie issue(u32 protocol_version, const HandshakeNonce& nonce,
                               const crypto::Hash& token_hash, const Address& from,
                               core::MonoTime now) noexcept;
    // true khi `cookie` do jar này cấp cho đúng các trường và địa chỉ đó, bằng khoá hiện tại hay
    // khoá trước, và chưa hết hạn lúc `now`.
    [[nodiscard]] bool check(const Cookie& cookie, u32 protocol_version,
                             const HandshakeNonce& nonce, const crypto::Hash& token_hash,
                             const Address& from, core::MonoTime now) const noexcept;

private:
    crypto::AeadKey current_;
    crypto::AeadKey previous_;
    bool has_previous_ = false;
    u64 next_sequence_ = 0;
    core::MonoTime rotated_at_;
};

}  // namespace orion::net
