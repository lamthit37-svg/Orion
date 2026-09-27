#pragma once

// UTF-8 là mã hoá chuỗi ở mọi nơi (CLAUDE.md X.3). Các hàm ở đây kiểm và cắt chuỗi UTF-8 mà không
// cấp phát; chúng dùng cho dữ liệu từ ngoài (tên nhân vật, chat, trường JSON) và cho log.

#include "engine/core/types.hpp"

#include <string_view>

namespace orion::core {

// UTF-8 chặt theo RFC 3629: từ chối mã hoá dài hơn cần thiết, surrogate U+D800..U+DFFF, điểm mã
// trên U+10FFFF, và chuỗi byte bị cắt giữa chừng. O(n).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Số điểm mã. Tiền điều kiện: is_valid_utf8(text). O(n).
[[nodiscard]] usize utf8_length(std::string_view text) noexcept;

// Tiền tố dài nhất không quá `max_bytes` byte mà không cắt ngang một điểm mã.
// Tiền điều kiện: is_valid_utf8(text). O(1): lùi tối đa 3 byte.
[[nodiscard]] std::string_view utf8_truncate(std::string_view text, usize max_bytes) noexcept;

}  // namespace orion::core
