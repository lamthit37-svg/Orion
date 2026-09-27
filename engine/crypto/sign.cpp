#include "engine/crypto/sign.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"

#include <sodium/crypto_sign.h>

#include <cstddef>
#include <span>

namespace orion::crypto {
namespace {

static_assert(crypto_sign_PUBLICKEYBYTES == kSigningPublicKeySize);
static_assert(crypto_sign_SECRETKEYBYTES == kSigningSecretKeySize);
static_assert(crypto_sign_SEEDBYTES == kSigningSeedSize);
static_assert(crypto_sign_BYTES == kSignatureSize);

}  // namespace

SigningKeyPair generate_signing_key_pair() noexcept {
    SigningKeyPair pair;
    const int status =
        crypto_sign_keypair(core::as_writable_uchars(pair.public_key.bytes).data(),
                            core::as_writable_uchars(pair.secret_key.mutable_view()).data());
    ORION_VERIFY(status == 0, "crypto_sign_keypair trả {}", status);
    return pair;
}

SigningKeyPair signing_key_pair_from_seed(const SigningSeed& seed) noexcept {
    SigningKeyPair pair;
    const int status =
        crypto_sign_seed_keypair(core::as_writable_uchars(pair.public_key.bytes).data(),
                                 core::as_writable_uchars(pair.secret_key.mutable_view()).data(),
                                 core::as_uchars(seed.view()).data());
    ORION_VERIFY(status == 0, "crypto_sign_seed_keypair trả {}", status);
    return pair;
}

Signature sign(const std::span<const std::byte> message,
               const SigningSecretKey& secret_key) noexcept {
    Signature signature;
    const int status = crypto_sign_detached(
        core::as_writable_uchars(signature.bytes).data(), nullptr, core::as_uchars(message).data(),
        message.size(), core::as_uchars(secret_key.view()).data());
    ORION_VERIFY(status == 0, "crypto_sign_detached trả {}", status);
    return signature;
}

bool verify(const std::span<const std::byte> message, const Signature& signature,
            const SigningPublicKey& public_key) noexcept {
    return crypto_sign_verify_detached(core::as_uchars(signature.bytes).data(),
                                       core::as_uchars(message).data(), message.size(),
                                       core::as_uchars(public_key.bytes).data()) == 0;
}

}  // namespace orion::crypto
