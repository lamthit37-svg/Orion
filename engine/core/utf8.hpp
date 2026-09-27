#pragma once

// UTF-8 là mã hoá chuỗi ở mọi nơi (CLAUDE.md X.3). Các hàm ở đây kiểm và cắt chuỗi UTF-8 mà không
// cấp phát; chúng dùng cho dữ liệu từ ngoài (tên nhân vật, chat, trường JSON) và cho log.

#include "engine/core/types.hpp"

#include <string_view>

namespace orion::core {

// UTF-8 chặt theo RFC 3629: từ chối mã hoá dài hơn cần thiết, surrogate U+D800..U+DFFF, điểm mã
// trên U+10FFFF, và chuỗi byte bị cắt giữa chừng. O(n).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Độ dài của chuỗi byte UTF-8 hợp lệ bắt đầu tại `offset`, hoặc 0 nếu byte ở đó không mở đầu một
// điểm mã hợp lệ (dùng để thay từng đoạn hỏng bằng U+FFFD khi ghi JSON). Tiền điều kiện:
// offset < text.size(). O(1).
[[nodiscard]] usize utf8_sequence_length(std::string_view text, usize offset) noexcept;

// Số điểm mã. Tiền điều kiện: is_valid_utf8(text). O(n).
[[nodiscard]] usize utf8_length(std::string_view text) noexcept;

// Tiền tố dài nhất không quá `max_bytes` byte mà không cắt ngang một điểm mã.
// Tiền điều kiện: is_valid_utf8(text). O(1): lùi tối đa 3 byte.
[[nodiscard]] std::string_view utf8_truncate(std::string_view text, usize max_bytes) noexcept;

// Bỏ điểm mã dở dang ở cuối `text` (ví dụ khi bộ đệm cắt ngang chuỗi byte của một điểm mã); phần
// trước đó giữ nguyên. O(1).
[[nodiscard]] std::string_view utf8_drop_incomplete_tail(std::string_view text) noexcept;

}  // namespace orion::core
