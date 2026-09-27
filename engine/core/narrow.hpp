#pragma once

// Chuyển kiểu số có thể làm mất giá trị (CLAUDE.md X.3).
//
// - orion::narrow<T>(v): dùng khi người gọi biết chắc giá trị vừa kiểu đích; assert khi sai
//   (lỗi lập trình, X.5). Ở ship chỉ còn static_cast.
// - orion::try_narrow<T>(v): dùng cho dữ liệu từ ngoài; trả std::nullopt khi giá trị không vừa, để
//   người gọi trả Error.
//
// "Vừa" nghĩa là đổi sang kiểu đích rồi đổi ngược lại được đúng giá trị cũ, không đổi dấu. NaN và
// vô cực giữa hai kiểu số thực được coi là vừa. Mọi kiểm tra chạy trước khi ép kiểu, vì ép số thực
// ngoài khoảng của kiểu đích là UB.

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"

#include <bit>
#include <concepts>
#include <limits>
#include <optional>
#include <source_location>
#include <type_traits>

namespace orion {
namespace detail {

template <class T>
concept Arithmetic = std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool> &&
                     !std::is_same_v<std::remove_cv_t<T>, long double>;

// Nhận NaN qua bit thay vì `v != v`: không phụ thuộc <cmath> (chưa constexpr trên mọi toolchain,
// NGHI-NGO-019) và không bị clang-tidy coi là biểu thức thừa.
template <std::floating_point F>
[[nodiscard]] constexpr bool is_nan(const F value) noexcept {
    if constexpr (sizeof(F) == sizeof(u32)) {
        const auto bits = std::bit_cast<u32>(value);
        return (bits & 0x7F80'0000U) == 0x7F80'0000U && (bits & 0x007F'FFFFU) != 0;
    } else {
        static_assert(sizeof(F) == sizeof(u64));
        const auto bits = std::bit_cast<u64>(value);
        return (bits & 0x7FF0'0000'0000'0000ULL) == 0x7FF0'0000'0000'0000ULL &&
               (bits & 0x000F'FFFF'FFFF'FFFFULL) != 0;
    }
}

// 2 mũ n, tính đúng tuyệt đối trong kiểu số thực F với n trong khoảng số mũ của F.
template <std::floating_point F>
[[nodiscard]] consteval F power_of_two(const int exponent) noexcept {
    F value = 1;
    for (int i = 0; i < exponent; ++i) {
        value *= 2;
    }
    return value;
}

// Số thực `value` có ép sang số nguyên I mà không UB không: phải nằm trong [thấp, cao) với cả hai
// cận là lũy thừa của 2, biểu diễn đúng trong F. NaN không nằm trong khoảng nào.
template <std::integral I, std::floating_point F>
[[nodiscard]] constexpr bool float_in_integer_range(const F value) noexcept {
    constexpr F kUpper = power_of_two<F>(std::numeric_limits<I>::digits);
    constexpr F kLower = std::is_signed_v<I> ? -kUpper : F{0};
    return value >= kLower && value < kUpper;
}

// Kiểm khoảng trước khi ép, vì ép số thực ra ngoài khoảng của kiểu đích là UB.
template <Arithmetic To, Arithmetic From>
[[nodiscard]] constexpr bool in_range_for_cast(const From value) noexcept {
    if constexpr (std::floating_point<From> && std::floating_point<To>) {
        if constexpr (std::numeric_limits<To>::max() < std::numeric_limits<From>::max()) {
            const bool finite = !is_nan(value) && value <= std::numeric_limits<From>::max() &&
                                value >= std::numeric_limits<From>::lowest();
            // NaN và vô cực đổi sang kiểu số thực khác vẫn là chính nó.
            return !finite || (value <= static_cast<From>(std::numeric_limits<To>::max()) &&
                               value >= static_cast<From>(std::numeric_limits<To>::lowest()));
        }
        return true;
    } else if constexpr (std::floating_point<From>) {
        return float_in_integer_range<To>(value);
    } else if constexpr (std::floating_point<To>) {
        // Số nguyên đổi sang số thực có thể làm tròn lên đúng cận trên (u64 max thành 2^64), nên
        // phải kiểm kết quả trước khi đổi ngược lại.
        return float_in_integer_range<From>(static_cast<To>(value));
    } else {
        return true;  // Số nguyên sang số nguyên luôn xác định (C++20: theo mô-đun).
    }
}

template <Arithmetic To, Arithmetic From>
[[nodiscard]] constexpr bool fits(const From value) noexcept {
    if (!in_range_for_cast<To>(value)) {
        return false;
    }
    const To converted = static_cast<To>(value);
    if constexpr (std::floating_point<From> && std::floating_point<To>) {
        if (is_nan(value)) {
            return is_nan(converted);
        }
    }
    if (static_cast<From>(converted) != value) {
        return false;
    }
    if constexpr (std::is_signed_v<To> != std::is_signed_v<From>) {
        return (converted < To{}) == (value < From{});
    }
    return true;
}

}  // namespace detail

// Ép kiểu mà người gọi biết chắc không mất giá trị; assert (X.5) nếu mất. `where` mặc định là chỗ
// gọi, để thông điệp assert chỉ đúng dòng của người gọi.
template <detail::Arithmetic To, detail::Arithmetic From>
[[nodiscard]] constexpr To narrow(const From value, const std::source_location& where =
                                                        std::source_location::current()) noexcept {
#if !ORION_SHIP
    if (!detail::fits<To>(value)) {
        detail::assert_failed_format("orion::narrow", "giá trị không vừa kiểu đích", where,
                                     "ép kiểu làm mất giá trị");
    }
#else
    static_cast<void>(where);
#endif
    return static_cast<To>(value);
}

// Ép kiểu dữ liệu từ ngoài: std::nullopt nếu giá trị không vừa kiểu đích.
template <detail::Arithmetic To, detail::Arithmetic From>
[[nodiscard]] constexpr std::optional<To> try_narrow(const From value) noexcept {
    if (!detail::fits<To>(value)) {
        return std::nullopt;
    }
    return static_cast<To>(value);
}

}  // namespace orion
