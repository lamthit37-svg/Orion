#pragma once

// Hàm số học vô hướng tất định (CLAUDE.md X.11).
//
// game/shared không được include <cmath>; nó dùng các hàm ở đây. Header này cũng không include
// <cmath>, nên code include nó không vô tình thấy std::sin hay std::pow. sqrt là phép IEEE 754 làm
// tròn đúng; floor, ceil, trunc, round và fmod cho kết quả đúng tuyệt đối. Vì vậy chúng cho cùng
// từng bit trên mọi toolchain khi không bật -ffast-math, và được cài đặt trong scalar.cpp bằng hàm
// của thư viện chuẩn.

#include "engine/core/types.hpp"

#include <bit>
#include <concepts>
#include <numbers>
#include <type_traits>

namespace orion::math {

// π làm tròn về f64 và f32; nhân, chia cho 2 là chính xác.
inline constexpr f64 kPi = std::numbers::pi;
inline constexpr f64 kHalfPi = std::numbers::pi / 2;
inline constexpr f64 kTau = 2 * std::numbers::pi;
inline constexpr f32 kPiF = std::numbers::pi_v<f32>;
inline constexpr f32 kHalfPiF = std::numbers::pi_v<f32> / 2;
inline constexpr f32 kTauF = 2 * std::numbers::pi_v<f32>;

namespace detail {

template <class T>
struct FloatBits;

template <>
struct FloatBits<f32> {
    using Type = u32;
    static constexpr u32 kSign = 0x8000'0000U;
    static constexpr u32 kExponent = 0x7F80'0000U;
    static constexpr u32 kMantissa = 0x007F'FFFFU;
};

template <>
struct FloatBits<f64> {
    using Type = u64;
    static constexpr u64 kSign = 0x8000'0000'0000'0000ULL;
    static constexpr u64 kExponent = 0x7FF0'0000'0000'0000ULL;
    static constexpr u64 kMantissa = 0x000F'FFFF'FFFF'FFFFULL;
};

template <class T>
concept Float = std::same_as<T, f32> || std::same_as<T, f64>;

}  // namespace detail

// Các hàm phân loại dưới đây đọc bit trực tiếp, nên vẫn đúng khi compiler được phép giả định không
// có NaN, và dùng được lúc biên dịch.
template <detail::Float T>
[[nodiscard]] constexpr bool is_nan(const T value) noexcept {
    using Bits = detail::FloatBits<T>;
    const auto bits = std::bit_cast<typename Bits::Type>(value);
    return (bits & Bits::kExponent) == Bits::kExponent && (bits & Bits::kMantissa) != 0;
}

template <detail::Float T>
[[nodiscard]] constexpr bool is_finite(const T value) noexcept {
    using Bits = detail::FloatBits<T>;
    return (std::bit_cast<typename Bits::Type>(value) & Bits::kExponent) != Bits::kExponent;
}

// true với số âm, -0.0 và NaN có bit dấu.
template <detail::Float T>
[[nodiscard]] constexpr bool sign_bit(const T value) noexcept {
    using Bits = detail::FloatBits<T>;
    return (std::bit_cast<typename Bits::Type>(value) & Bits::kSign) != 0;
}

// Trị tuyệt đối bằng cách xoá bit dấu: đúng cả với -0.0 và NaN.
template <detail::Float T>
[[nodiscard]] constexpr T abs(const T value) noexcept {
    using Bits = detail::FloatBits<T>;
    using U = typename Bits::Type;
    return std::bit_cast<T>(static_cast<U>(std::bit_cast<U>(value) & ~Bits::kSign));
}

// Độ lớn của magnitude với dấu của sign_source.
template <detail::Float T>
[[nodiscard]] constexpr T copy_sign(const T magnitude, const T sign_source) noexcept {
    using Bits = detail::FloatBits<T>;
    using U = typename Bits::Type;
    const U bits = (std::bit_cast<U>(magnitude) & ~Bits::kSign) |
                   (std::bit_cast<U>(sign_source) & Bits::kSign);
    return std::bit_cast<T>(bits);
}

// min, max, clamp cho số: trả a khi so sánh không xác định (có NaN).
template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T min(const T a, const T b) noexcept {
    return b < a ? b : a;
}

template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T max(const T a, const T b) noexcept {
    return a < b ? b : a;
}

// Tiền điều kiện: lo <= hi.
template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T clamp(const T value, const T lo, const T hi) noexcept {
    if (value < lo) {
        return lo;
    }
    if (hi < value) {
        return hi;
    }
    return value;
}

// a + (b - a) * t, đúng thứ tự này ở mọi toolchain (không hợp nhất FMA, X.11). Không đảm bảo trả
// đúng b khi t = 1; code cần điều đó thì xử lý riêng.
template <detail::Float T>
[[nodiscard]] constexpr T lerp(const T a, const T b, const T t) noexcept {
    return a + ((b - a) * t);
}

[[nodiscard]] f32 sqrt(f32 value) noexcept;
[[nodiscard]] f64 sqrt(f64 value) noexcept;
[[nodiscard]] f32 floor(f32 value) noexcept;
[[nodiscard]] f64 floor(f64 value) noexcept;
[[nodiscard]] f32 ceil(f32 value) noexcept;
[[nodiscard]] f64 ceil(f64 value) noexcept;
[[nodiscard]] f32 trunc(f32 value) noexcept;
[[nodiscard]] f64 trunc(f64 value) noexcept;
// Làm tròn nửa ra xa 0, như std::round.
[[nodiscard]] f32 round(f32 value) noexcept;
[[nodiscard]] f64 round(f64 value) noexcept;
// x - n·y với n là thương cắt về 0, như std::fmod; kết quả cùng dấu với x.
[[nodiscard]] f32 fmod(f32 x, f32 y) noexcept;
[[nodiscard]] f64 fmod(f64 x, f64 y) noexcept;

}  // namespace orion::math
