#pragma once

// So số thực theo ulp cho test của engine/math và các module dùng nó.

#include "engine/core/types.hpp"

#include <bit>
#include <limits>

namespace orion::math::testing {

// Ánh xạ bit của số thực sang số nguyên có cùng thứ tự; +0 và -0 cùng về 0.
[[nodiscard]] inline i64 ordered_bits(const f64 value) {
    const auto bits = std::bit_cast<i64>(value);
    return bits >= 0 ? bits : std::numeric_limits<i64>::min() - bits;
}

[[nodiscard]] inline i64 ordered_bits(const f32 value) {
    const auto bits = std::bit_cast<i32>(value);
    return bits >= 0 ? bits : i64{std::numeric_limits<i32>::min()} - bits;
}

// Số bước giữa a và b trên dãy các giá trị biểu diễn được. Không dùng cho NaN.
template <class T>
[[nodiscard]] u64 ulp_distance(const T a, const T b) {
    const i64 oa = ordered_bits(a);
    const i64 ob = ordered_bits(b);
    return oa >= ob ? static_cast<u64>(oa) - static_cast<u64>(ob)
                    : static_cast<u64>(ob) - static_cast<u64>(oa);
}

}  // namespace orion::math::testing
