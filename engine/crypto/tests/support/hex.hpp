#pragma once

// Đổi chuỗi hex của test vector sang byte, cho test của engine/crypto.

#include "engine/core/types.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace orion::crypto::testing {

// Chuỗi hex hợp lệ, độ dài chẵn (test vector viết tay; sai là lỗi của test).
[[nodiscard]] inline std::vector<std::byte> from_hex(const std::string_view hex) {
    const auto nibble = [](const char c) -> u8 {
        if (c >= '0' && c <= '9') {
            return static_cast<u8>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<u8>(c - 'a' + 10);
        }
        return static_cast<u8>(c - 'A' + 10);
    };
    std::vector<std::byte> bytes;
    bytes.reserve(hex.size() / 2);
    for (usize i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::byte>((nibble(hex[i]) << 4U) | nibble(hex[i + 1])));
    }
    return bytes;
}

[[nodiscard]] inline std::vector<std::byte> as_byte_vector(const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

template <class Fixed>
[[nodiscard]] Fixed fixed_from_hex(const std::string_view hex) {
    const std::vector<std::byte> bytes = from_hex(hex);
    Fixed value;
    for (usize i = 0; i < Fixed::kSize && i < bytes.size(); ++i) {
        value.bytes[i] = bytes[i];
    }
    return value;
}

template <class Secret>
[[nodiscard]] Secret secret_from_hex(const std::string_view hex) {
    const std::vector<std::byte> bytes = from_hex(hex);
    Secret value;
    const auto out = value.mutable_view();
    for (usize i = 0; i < Secret::kSize && i < bytes.size(); ++i) {
        out[i] = bytes[i];
    }
    return value;
}

}  // namespace orion::crypto::testing
