#include "engine/math/quat.hpp"

#include "engine/core/types.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/trig.hpp"
#include "engine/math/vec.hpp"

namespace orion::math {
namespace {

[[nodiscard]] constexpr Quat scale(const Quat q, const f32 s) noexcept {
    return {q.x * s, q.y * s, q.z * s, q.w * s};
}

[[nodiscard]] constexpr Quat add(const Quat a, const Quat b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

// Trên cos θ này (hai quaternion cách nhau dưới 1.8° trên S³, tức góc quay dưới 3.6°), sin θ quá
// nhỏ để chia; slerp dùng nlerp. Ở đúng ngưỡng, nlerp lệch slerp tối đa 5.07e-7 rad trên S³ (tính
// giải tích), tức 1.02e-6 rad góc quay (đo: 1.019e-6, quat_test.cpp giữ dưới 1.2e-6).
constexpr f32 kSlerpLinearThreshold = 0.9995F;

}  // namespace

Quat from_axis_angle(const Vec3 axis, const f32 angle) noexcept {
    const SinCosF half = sincos(angle * 0.5F);
    return {axis.x * half.sin, axis.y * half.sin, axis.z * half.sin, half.cos};
}

Quat from_yaw(const f32 yaw) noexcept {
    return from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, yaw);
}

Quat normalize_or_identity(const Quat q) noexcept {
    const f32 len = sqrt(dot(q, q));
    if (!(len > 0.0F) || !is_finite(len)) {
        return Quat::identity();
    }
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

Quat nlerp(const Quat a, const Quat b, const f32 t) noexcept {
    // q và -q là cùng một phép quay; đổi dấu b để đi đường ngắn.
    const Quat target = dot(a, b) < 0.0F ? -b : b;
    return normalize_or_identity(add(scale(a, 1.0F - t), scale(target, t)));
}

Quat slerp(const Quat a, const Quat b, const f32 t) noexcept {
    f32 cos_theta = dot(a, b);
    Quat target = b;
    if (cos_theta < 0.0F) {
        target = -b;
        cos_theta = -cos_theta;
    }
    if (cos_theta > kSlerpLinearThreshold) {
        return nlerp(a, target, t);
    }
    const f32 theta = acos(cos_theta);
    const f32 sin_theta = sin(theta);
    const f32 weight_a = sin((1.0F - t) * theta) / sin_theta;
    const f32 weight_b = sin(t * theta) / sin_theta;
    return add(scale(a, weight_a), scale(target, weight_b));
}

}  // namespace orion::math
