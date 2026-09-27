#pragma once

// SipHash-2-4 có khoá (crypto_shorthash của libsodium): hash 64 bit cho bảng băm mà khoá đến từ
// mạng, ví dụ địa chỉ nguồn trong bộ giới hạn tần suất của engine/net. Khoá ngẫu nhiên và bí mật,
// nên kẻ gửi gói không dựng được các khoá đụng nhau để dồn mọi thứ vào một ô (hash flooding).
// Không phải MAC và không thay cho hash.hpp.

#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"

#include <cstddef>
#include <span>

namespace orion::crypto {

inline constexpr usize kShortHashKeySize = 16;

struct ShortHashKeyTag;
using ShortHashKey = SecretBytes<kShortHashKeySize, ShortHashKeyTag>;

[[nodiscard]] ShortHashKey generate_short_hash_key() noexcept;

// 8 byte ra của SipHash-2-4, đọc little-endian như bản tham chiếu.
[[nodiscard]] u64 short_hash(std::span<const std::byte> data, const ShortHashKey& key) noexcept;

}  // namespace orion::crypto
