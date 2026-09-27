#pragma once

// Vector 2, 3, 4 chiều (ARCH §2). Vec2, Vec3, Vec4 dùng f32 cho độ lệch cục bộ, vận tốc, pháp
// tuyến; DVec3 dùng f64 cho hiệu hai WorldPos (ADR 0001).
//
// Mọi phép tính viết theo thứ tự trái sang phải cố định, không hợp nhất FMA (X.11), nên cho cùng
// từng bit trên mọi toolchain.

#include "engine/core/types.hpp"
#include "engine/math/scalar.hpp"

#include <type_traits>

namespace orion::math {

template <detail::Float T>
struct TVec2 {
    using value_type = T;

    T x{};
    T y{};

    [[nodiscard]] friend constexpr TVec2 operator+(const TVec2 a, const TVec2 b) noexcept {
        return {a.x + b.x, a.y + b.y};
    }
    [[nodiscard]] friend constexpr TVec2 operator-(const TVec2 a, const TVec2 b) noexcept {
        return {a.x - b.x, a.y - b.y};
    }
    [[nodiscard]] friend constexpr TVec2 operator-(const TVec2 v) noexcept { return {-v.x, -v.y}; }
    [[nodiscard]] friend constexpr TVec2 operator*(const TVec2 v, const T s) noexcept {
        return {v.x * s, v.y * s};
    }
    [[nodiscard]] friend constexpr TVec2 operator*(const T s, const TVec2 v) noexcept {
        return v * s;
    }
    [[nodiscard]] friend constexpr TVec2 operator/(const TVec2 v, const T s) noexcept {
        return {v.x / s, v.y / s};
    }
    constexpr TVec2& operator+=(const TVec2 other) noexcept { return *this = *this + other; }
    constexpr TVec2& operator-=(const TVec2 other) noexcept { return *this = *this - other; }
    constexpr TVec2& operator*=(const T s) noexcept { return *this = *this * s; }
    constexpr TVec2& operator/=(const T s) noexcept { return *this = *this / s; }
    [[nodiscard]] friend constexpr bool operator==(const TVec2&, const TVec2&) noexcept = default;
};

template <detail::Float T>
struct TVec3 {
    using value_type = T;

    T x{};
    T y{};
    T z{};

    [[nodiscard]] friend constexpr TVec3 operator+(const TVec3 a, const TVec3 b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    [[nodiscard]] friend constexpr TVec3 operator-(const TVec3 a, const TVec3 b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    [[nodiscard]] friend constexpr TVec3 operator-(const TVec3 v) noexcept {
        return {-v.x, -v.y, -v.z};
    }
    [[nodiscard]] friend constexpr TVec3 operator*(const TVec3 v, const T s) noexcept {
        return {v.x * s, v.y * s, v.z * s};
    }
    [[nodiscard]] friend constexpr TVec3 operator*(const T s, const TVec3 v) noexcept {
        return v * s;
    }
    [[nodiscard]] friend constexpr TVec3 operator/(const TVec3 v, const T s) noexcept {
        return {v.x / s, v.y / s, v.z / s};
    }
    constexpr TVec3& operator+=(const TVec3 other) noexcept { return *this = *this + other; }
    constexpr TVec3& operator-=(const TVec3 other) noexcept { return *this = *this - other; }
    constexpr TVec3& operator*=(const T s) noexcept { return *this = *this * s; }
    constexpr TVec3& operator/=(const T s) noexcept { return *this = *this / s; }
    [[nodiscard]] friend constexpr bool operator==(const TVec3&, const TVec3&) noexcept = default;
};

template <detail::Float T>
struct TVec4 {
    using value_type = T;

    T x{};
    T y{};
    T z{};
    T w{};

    [[nodiscard]] friend constexpr TVec4 operator+(const TVec4 a, const TVec4 b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    [[nodiscard]] friend constexpr TVec4 operator-(const TVec4 a, const TVec4 b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    [[nodiscard]] friend constexpr TVec4 operator-(const TVec4 v) noexcept {
        return {-v.x, -v.y, -v.z, -v.w};
    }
    [[nodiscard]] friend constexpr TVec4 operator*(const TVec4 v, const T s) noexcept {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    [[nodiscard]] friend constexpr TVec4 operator*(const T s, const TVec4 v) noexcept {
        return v * s;
    }
    [[nodiscard]] friend constexpr TVec4 operator/(const TVec4 v, const T s) noexcept {
        return {v.x / s, v.y / s, v.z / s, v.w / s};
    }
    constexpr TVec4& operator+=(const TVec4 other) noexcept { return *this = *this + other; }
    constexpr TVec4& operator-=(const TVec4 other) noexcept { return *this = *this - other; }
    constexpr TVec4& operator*=(const T s) noexcept { return *this = *this * s; }
    constexpr TVec4& operator/=(const T s) noexcept { return *this = *this / s; }
    [[nodiscard]] friend constexpr bool operator==(const TVec4&, const TVec4&) noexcept = default;
};

using Vec2 = TVec2<f32>;
using Vec3 = TVec3<f32>;
using Vec4 = TVec4<f32>;
using DVec2 = TVec2<f64>;
using DVec3 = TVec3<f64>;

template <detail::Float T>
[[nodiscard]] constexpr T dot(const TVec2<T> a, const TVec2<T> b) noexcept {
    return (a.x * b.x) + (a.y * b.y);
}

template <detail::Float T>
[[nodiscard]] constexpr T dot(const TVec3<T> a, const TVec3<T> b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

template <detail::Float T>
[[nodiscard]] constexpr T dot(const TVec4<T> a, const TVec4<T> b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

// Tích có hướng theo hệ tay phải (ADR 0001): cross(+X, +Y) = +Z.
template <detail::Float T>
[[nodiscard]] constexpr TVec3<T> cross(const TVec3<T> a, const TVec3<T> b) noexcept {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

namespace detail {

template <class V>
struct IsVector : std::false_type {};
template <class T>
struct IsVector<TVec2<T>> : std::true_type {};
template <class T>
struct IsVector<TVec3<T>> : std::true_type {};
template <class T>
struct IsVector<TVec4<T>> : std::true_type {};

}  // namespace detail

template <class V>
concept Vector = detail::IsVector<V>::value;

template <Vector V>
[[nodiscard]] constexpr typename V::value_type length_squared(const V v) noexcept {
    return dot(v, v);
}

template <Vector V>
[[nodiscard]] typename V::value_type length(const V v) noexcept {
    return sqrt(dot(v, v));
}

template <Vector V>
[[nodiscard]] constexpr typename V::value_type distance_squared(const V a, const V b) noexcept {
    return length_squared(a - b);
}

template <Vector V>
[[nodiscard]] typename V::value_type distance(const V a, const V b) noexcept {
    return length(a - b);
}

// Vector cùng hướng, độ dài 1. Trả vector 0 khi độ dài bằng 0 hoặc không hữu hạn, kể cả khi bình
// phương độ dài tràn số hay bị làm tròn về 0.
template <Vector V>
[[nodiscard]] V normalize_or_zero(const V v) noexcept {
    using T = typename V::value_type;
    const T len = length(v);
    if (!(len > T{0}) || !is_finite(len)) {
        return V{};
    }
    return v / len;
}

template <Vector V>
[[nodiscard]] constexpr V lerp(const V a, const V b, const typename V::value_type t) noexcept {
    return a + ((b - a) * t);
}

template <detail::Float T>
[[nodiscard]] constexpr TVec3<T> min(const TVec3<T> a, const TVec3<T> b) noexcept {
    return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)};
}

template <detail::Float T>
[[nodiscard]] constexpr TVec3<T> max(const TVec3<T> a, const TVec3<T> b) noexcept {
    return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)};
}

template <detail::Float T>
[[nodiscard]] constexpr TVec3<T> abs(const TVec3<T> v) noexcept {
    return {abs(v.x), abs(v.y), abs(v.z)};
}

}  // namespace orion::math
