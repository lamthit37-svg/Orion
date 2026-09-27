#include "engine/crypto/aead.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <sodium/crypto_aead_chacha20poly1305.h>

#include <cstddef>
#include <span>

namespace orion::crypto {
namespace {

static_assert(crypto_aead_chacha20poly1305_ietf_KEYBYTES == kAeadKeySize);
static_assert(crypto_aead_chacha20poly1305_ietf_NPUBBYTES == kAeadNonceSize);
static_assert(crypto_aead_chacha20poly1305_ietf_ABYTES == kAeadTagSize);

}  // namespace

AeadKey generate_aead_key() noexcept {
    AeadKey key;
    crypto_aead_chacha20poly1305_ietf_keygen(core::as_writable_uchars(key.mutable_view()).data());
    return key;
}

AeadNonce nonce_from_sequence(const u32 channel, const u64 sequence) noexcept {
    AeadNonce nonce;
    const std::span<std::byte, kAeadNonceSize> bytes = nonce.bytes;
    core::store_le<u32>(bytes.first<4>(), channel);
    core::store_le<u64>(bytes.last<8>(), sequence);
    return nonce;
}

Result<usize> seal(const std::span<std::byte> out, const std::span<const std::byte> plaintext,
                   const std::span<const std::byte> associated_data, const AeadNonce& nonce,
                   const AeadKey& key) noexcept {
    // Quá cỡ này libsodium gọi sodium_misuse() và dừng tiến trình; trả lỗi trước khi tới đó. Cũng
    // nhờ vậy phép cộng bên dưới không tràn.
    if (plaintext.size() > crypto_aead_chacha20poly1305_ietf_MESSAGEBYTES_MAX) {
        return fail(ErrorCode::InvalidArgument, "aead: plaintext quá dài");
    }
    if (out.size() < plaintext.size() + kAeadTagSize) {
        return fail(ErrorCode::InvalidArgument, "aead: buffer ra thiếu chỗ cho tag",
                    static_cast<i64>(out.size()));
    }
    unsigned long long written = 0;
    const int status = crypto_aead_chacha20poly1305_ietf_encrypt(
        core::as_writable_uchars(out).data(), &written, core::as_uchars(plaintext).data(),
        plaintext.size(), core::as_uchars(associated_data).data(), associated_data.size(), nullptr,
        core::as_uchars(nonce.bytes).data(), core::as_uchars(key.view()).data());
    ORION_VERIFY(status == 0, "crypto_aead_chacha20poly1305_ietf_encrypt trả {}", status);
    return static_cast<usize>(written);
}

Result<usize> open(const std::span<std::byte> out, const std::span<const std::byte> ciphertext,
                   const std::span<const std::byte> associated_data, const AeadNonce& nonce,
                   const AeadKey& key) noexcept {
    if (ciphertext.size() < kAeadTagSize) {
        return fail(ErrorCode::DataLoss, "aead: gói ngắn hơn tag",
                    static_cast<i64>(ciphertext.size()));
    }
    if (out.size() < ciphertext.size() - kAeadTagSize) {
        return fail(ErrorCode::InvalidArgument, "aead: buffer ra thiếu chỗ",
                    static_cast<i64>(out.size()));
    }
    unsigned long long written = 0;
    const int status = crypto_aead_chacha20poly1305_ietf_decrypt(
        core::as_writable_uchars(out).data(), &written, nullptr, core::as_uchars(ciphertext).data(),
        ciphertext.size(), core::as_uchars(associated_data).data(), associated_data.size(),
        core::as_uchars(nonce.bytes).data(), core::as_uchars(key.view()).data());
    if (status != 0) {
        return fail(ErrorCode::DataLoss, "aead: xác thực thất bại");
    }
    return static_cast<usize>(written);
}

}  // namespace orion::crypto
