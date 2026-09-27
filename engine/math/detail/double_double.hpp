#pragma once

// Biến đổi không sai số (error-free transformation) trên f64, nền cho lượng giác chính xác dưới
// một ulp trong trig.cpp. Chỉ dùng cộng, trừ, nhân, chia IEEE theo thứ tự cố định và không cần FMA,
// nên tất định trên mọi toolchain (X.11) và chạy được lúc biên dịch.
//
// Một DoubleDouble biểu diễn giá trị hi + lo, với |lo| <= ulp(hi) / 2 sau khi chuẩn hoá. Nguồn:
// Dekker 1971, Knuth TAOCP tập 2 §4.2.2, Shewchuk 1997 ("Adaptive Precision Floating-Point
// Arithmetic").

#include "engine/core/types.hpp"

namespace orion::math::detail {

struct DoubleDouble {
    f64 hi;
    f64 lo;
};

// a + b = hi + lo chính xác. Tiền điều kiện: a = 0 hoặc số mũ của a không nhỏ hơn của b.
[[nodiscard]] constexpr DoubleDouble fast_two_sum(const f64 a, const f64 b) noexcept {
    const f64 sum = a + b;
    return {sum, b - (sum - a)};
}

// a + b = hi + lo chính xác, không tiền điều kiện về độ lớn (Knuth).
[[nodiscard]] constexpr DoubleDouble two_sum(const f64 a, const f64 b) noexcept {
    const f64 sum = a + b;
    const f64 b_part = sum - a;
    const f64 a_part = sum - b_part;
    return {sum, (a - a_part) + (b - b_part)};
}

// Tách a thành hai nửa 26 bit để tích từng cặp nửa không làm tròn (Veltkamp).
[[nodiscard]] constexpr DoubleDouble split(const f64 a) noexcept {
    constexpr f64 kSplitter = 134'217'729.0;  // 2^27 + 1
    const f64 t = kSplitter * a;
    const f64 high = t - (t - a);
    return {high, a - high};
}

// a · b = hi + lo chính xác (Dekker, dạng của Shewchuk). Tiền điều kiện: |a|, |b| < 2^995 và tích
// không rơi vào vùng subnormal.
[[nodiscard]] constexpr DoubleDouble two_product(const f64 a, const f64 b) noexcept {
    const f64 product = a * b;
    const DoubleDouble as = split(a);
    const DoubleDouble bs = split(b);
    const f64 error1 = product - (as.hi * bs.hi);
    const f64 error2 = error1 - (as.lo * bs.hi);
    const f64 error3 = error2 - (as.hi * bs.lo);
    return {product, (as.lo * bs.lo) - error3};
}

// (n.hi + n.lo) / (d.hi + d.lo), kết quả đã chuẩn hoá. Sai số tương đối cỡ 2^-104. Tiền điều kiện:
// d.hi != 0, và điều kiện của two_product cho thương với d.hi.
[[nodiscard]] constexpr DoubleDouble divide(const DoubleDouble n, const DoubleDouble d) noexcept {
    const f64 quotient = n.hi / d.hi;
    const DoubleDouble back = two_product(quotient, d.hi);
    // n.hi - back.hi không làm tròn vì hai số gần nhau (Sterbenz).
    const f64 remainder = (((n.hi - back.hi) - back.lo) + n.lo) - (quotient * d.lo);
    return fast_two_sum(quotient, remainder / d.hi);
}

}  // namespace orion::math::detail
