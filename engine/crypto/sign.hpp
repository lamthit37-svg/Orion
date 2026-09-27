#pragma once

// Chữ ký Ed25519: connect token do auth ký (ARCH §4.6, X.9) và manifest của pak (ARCH §4.4).

#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"

#include <cstddef>
#include <span>

namespace orion::crypto {

inline constexpr usize kSigningPublicKeySize = 32;
inline constexpr usize kSigningSecretKeySize = 64;
inline constexpr usize kSigningSeedSize = 32;
inline constexpr usize kSignatureSize = 64;

struct SigningPublicKeyTag;
struct SigningSecretKeyTag;
struct SigningSeedTag;
struct SignatureTag;
using SigningPublicKey = PublicBytes<kSigningPublicKeySize, SigningPublicKeyTag>;
// Khoá bí mật theo dạng của libsodium: seed 32 byte nối khoá công khai.
using SigningSecretKey = SecretBytes<kSigningSecretKeySize, SigningSecretKeyTag>;
using SigningSeed = SecretBytes<kSigningSeedSize, SigningSeedTag>;
using Signature = PublicBytes<kSignatureSize, SignatureTag>;

struct SigningKeyPair {
    SigningPublicKey public_key;
    SigningSecretKey secret_key;
};

[[nodiscard]] SigningKeyPair generate_signing_key_pair() noexcept;

// Cùng seed luôn cho cùng cặp khoá: dùng khi khoá được lưu dưới dạng seed.
[[nodiscard]] SigningKeyPair signing_key_pair_from_seed(const SigningSeed& seed) noexcept;

[[nodiscard]] Signature sign(std::span<const std::byte> message,
                             const SigningSecretKey& secret_key) noexcept;

// true khi chữ ký hợp lệ cho đúng message và khoá công khai này. Dữ liệu từ ngoài đi thẳng vào
// được: sai định dạng chỉ cho false.
[[nodiscard]] bool verify(std::span<const std::byte> message, const Signature& signature,
                          const SigningPublicKey& public_key) noexcept;

}  // namespace orion::crypto
