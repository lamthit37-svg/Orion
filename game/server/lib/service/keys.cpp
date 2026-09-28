// Tệp khoá (docs/formats/service.md, mục "Khoá và bí mật"): 64 chữ số hex.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/sign.hpp"
#include "game/server/lib/service/detail/secret.hpp"
#include "game/server/lib/service/service.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace orion::service {
namespace {

constexpr usize kKeyBytes = 32;

[[nodiscard]] std::optional<u8> hex_value(const char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<u8>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<u8>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<u8>(c - 'A' + 10);
    }
    return std::nullopt;
}

// Đọc 32 byte từ tệp khoá; `out` là nơi bên gọi giữ và ghi 0 khi xong.
[[nodiscard]] Result<void> read_key(const std::string_view path, const Environment environment,
                                    std::array<std::byte, kKeyBytes>& out) {
    Result<std::string> text = detail::read_secret_file(path, environment);
    if (!text) {
        return std::unexpected(text.error());
    }
    Result<void> decoded{};
    if (text->size() != kKeyBytes * 2) {
        decoded = fail(ErrorCode::InvalidArgument, "service: tệp khoá phải có đúng 64 chữ số hex");
    }
    for (usize i = 0; decoded && i < kKeyBytes; ++i) {
        const std::optional<u8> high = hex_value((*text)[2 * i]);
        const std::optional<u8> low = hex_value((*text)[(2 * i) + 1]);
        if (!high || !low) {
            decoded = fail(ErrorCode::InvalidArgument, "service: tệp khoá có ký tự không phải hex");
            break;
        }
        out.at(i) = static_cast<std::byte>((*high << 4U) | *low);
    }
    detail::wipe(*text);
    return decoded;
}

}  // namespace

Result<crypto::SigningKeyPair> load_signing_key(const std::string_view path,
                                                const Environment environment) {
    std::array<std::byte, kKeyBytes> bytes{};
    const Result<void> read = read_key(path, environment, bytes);
    if (!read) {
        crypto::secure_zero(bytes);
        return std::unexpected(read.error());
    }
    const crypto::SigningSeed seed{std::span<const std::byte, kKeyBytes>(bytes)};
    crypto::secure_zero(bytes);
    return crypto::signing_key_pair_from_seed(seed);
}

Result<crypto::SigningPublicKey> load_public_key(const std::string_view path,
                                                 const Environment environment) {
    crypto::SigningPublicKey key;
    const Result<void> read = read_key(path, environment, key.bytes);
    if (!read) {
        return std::unexpected(read.error());
    }
    return key;
}

}  // namespace orion::service
