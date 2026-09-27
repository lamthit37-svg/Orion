#pragma once

// Trao khoá X25519 cho bắt tay của kênh realtime (X.9), theo crypto_kx của libsodium: mỗi bên có
// một cặp khoá X25519; từ bí mật chung và hai khoá công khai, BLAKE2b-512 sinh ra hai khoá phiên,
// một cho mỗi chiều. Chiều nhận của client là chiều gửi của server và ngược lại.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/crypto.hpp"

namespace orion::crypto {

inline constexpr usize kKeyExchangePublicKeySize = 32;
inline constexpr usize kKeyExchangeSecretKeySize = 32;

struct KeyExchangePublicKeyTag;
struct KeyExchangeSecretKeyTag;
using KeyExchangePublicKey = PublicBytes<kKeyExchangePublicKeySize, KeyExchangePublicKeyTag>;
using KeyExchangeSecretKey = SecretBytes<kKeyExchangeSecretKeySize, KeyExchangeSecretKeyTag>;

struct KeyExchangeKeyPair {
    KeyExchangePublicKey public_key;
    KeyExchangeSecretKey secret_key;
};

// Khoá AEAD của hai chiều một kết nối.
struct SessionKeys {
    AeadKey receive;
    AeadKey transmit;
};

[[nodiscard]] KeyExchangeKeyPair generate_key_exchange_key_pair() noexcept;

// Khoá công khai tính từ khoá bí mật (nhân với điểm gốc của Curve25519).
[[nodiscard]] KeyExchangeKeyPair key_exchange_key_pair_from_secret(
    const KeyExchangeSecretKey& secret_key) noexcept;

// Khoá công khai của bên kia đến từ mạng: khoá cho bí mật chung toàn 0 (điểm bậc thấp) trả
// InvalidArgument.
[[nodiscard]] Result<SessionKeys> client_session_keys(
    const KeyExchangeKeyPair& client, const KeyExchangePublicKey& server_public_key) noexcept;
[[nodiscard]] Result<SessionKeys> server_session_keys(
    const KeyExchangeKeyPair& server, const KeyExchangePublicKey& client_public_key) noexcept;

}  // namespace orion::crypto
