#pragma once

// Tên kiểu số của cả dự án (CLAUDE.md X.3). `int`, `long`, `unsigned` trần chỉ dùng khi API ngoài
// bắt buộc. Từ vựng nền này nằm thẳng trong namespace orion để mọi module viết ngắn gọn.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace orion {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

// Tất định (X.11) và định dạng trên dây (X.10) dựa vào số thực IEEE 754 đúng kích thước.
static_assert(std::numeric_limits<f32>::is_iec559 && sizeof(f32) == 4);
static_assert(std::numeric_limits<f64>::is_iec559 && sizeof(f64) == 8);
static_assert(sizeof(usize) == 8, "Orion chỉ build cho kiến trúc 64 bit");

}  // namespace orion
