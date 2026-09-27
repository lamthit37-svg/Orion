#pragma once

// Quaternion đơn vị cho phép quay (ADR 0001 mục 3): thành phần (x, y, z, w), w là phần vô hướng.
// Tích a * b là phép quay b trước rồi a: rotate(a * b, v) == rotate(a, rotate(b, v)).

#include "engine/core/types.hpp"
#include "engine/math/vec.hpp"

namespace orion::math {

struct Quat {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
    f32 w = 1.0F;

    [[nodiscard]] static constexpr Quat identity() noexcept { return {}; }

    // Tích Hamilton.
    [[nodiscard]] friend constexpr Quat operator*(const Quat a, const Quat b) noexcept {
        return {
            (a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
            (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
            (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
            (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z),
        };
    }
    [[nodiscard]] friend constexpr Quat operator-(const Quat q) noexcept {
        return {-q.x, -q.y, -q.z, -q.w};
    }
    [[nodiscard]] friend constexpr bool operator==(const Quat&, const Quat&) noexcept = default;
};

[[nodiscard]] constexpr f32 dot(const Quat a, const Quat b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

// Nghịch đảo của quaternion đơn vị.
[[nodiscard]] constexpr Quat conjugate(const Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

// Quay v bằng q. Tiền điều kiện: q là quaternion đơn vị.
[[nodiscard]] constexpr Vec3 rotate(const Quat q, const Vec3 v) noexcept {
    // v' = v + w·t + u × t với u = (x, y, z) và t = 2 · (u × v); ít phép nhân hơn q·v·q*.
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0F;
    return v + (t * q.w) + cross(u, t);
}

// Quay một góc angle (radian) quanh trục đơn vị axis, theo quy tắc tay phải.
[[nodiscard]] Quat from_axis_angle(Vec3 axis, f32 angle) noexcept;

// Quay quanh +Y: yaw 0 nhìn về +Z, yaw +π/2 đưa +Z về +X (ADR 0001).
[[nodiscard]] Quat from_yaw(f32 yaw) noexcept;

// Quaternion đơn vị cùng hướng; identity khi độ dài bằng 0 hoặc không hữu hạn.
[[nodiscard]] Quat normalize_or_identity(Quat q) noexcept;

// Nội suy theo đường ngắn nhất rồi chuẩn hoá. Nhanh hơn slerp nhưng tốc độ góc không đều.
[[nodiscard]] Quat nlerp(Quat a, Quat b, f32 t) noexcept;

// Nội suy cầu theo đường ngắn nhất, tốc độ góc đều. Tiền điều kiện: a, b là quaternion đơn vị.
[[nodiscard]] Quat slerp(Quat a, Quat b, f32 t) noexcept;

}  // namespace orion::math
