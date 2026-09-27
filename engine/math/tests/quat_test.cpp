#include "engine/math/quat.hpp"

#include "engine/core/types.hpp"
#include "engine/math/random.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/vec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace orion::math {
namespace {

constexpr Vec3 kX{1.0F, 0.0F, 0.0F};
constexpr Vec3 kY{0.0F, 1.0F, 0.0F};
constexpr Vec3 kZ{0.0F, 0.0F, 1.0F};

static_assert(Quat::identity() == Quat{0.0F, 0.0F, 0.0F, 1.0F});
static_assert(rotate(Quat::identity(), Vec3{1, 2, 3}) == Vec3{1, 2, 3});
static_assert(conjugate(Quat{1, 2, 3, 4}) == Quat{-1, -2, -3, 4});
static_assert(Quat{0, 0, 0, 1} * Quat{1, 2, 3, 4} == Quat{1, 2, 3, 4});
static_assert(dot(Quat{1, 2, 3, 4}, Quat{1, 1, 1, 1}) == 10.0F);

void expect_near(const Vec3 actual, const Vec3 expected, const f32 tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

// Góc quay (radian) giữa hai phép quay, tính bằng f64 qua độ dài dây cung trên S³, ổn định cả khi
// góc rất nhỏ.
[[nodiscard]] f64 rotation_angle_between(const Quat a, const Quat b) {
    const f64 sign = dot(a, b) < 0.0F ? -1.0 : 1.0;
    const f64 dx = f64{a.x} - (sign * f64{b.x});
    const f64 dy = f64{a.y} - (sign * f64{b.y});
    const f64 dz = f64{a.z} - (sign * f64{b.z});
    const f64 dw = f64{a.w} - (sign * f64{b.w});
    const f64 chord = std::sqrt((dx * dx) + (dy * dy) + (dz * dz) + (dw * dw));
    return 4.0 * std::asin(std::min(chord * 0.5, 1.0));
}

[[nodiscard]] Quat random_unit_quat(Pcg32& rng) {
    const Quat q{(rng.next_f32() * 2.0F) - 1.0F, (rng.next_f32() * 2.0F) - 1.0F,
                 (rng.next_f32() * 2.0F) - 1.0F, (rng.next_f32() * 2.0F) - 1.0F};
    return normalize_or_identity(q);
}

TEST(Quat, YawFollowsAdr0001) {
    // yaw 0 nhìn về +Z; yaw +π/2 đưa +Z về +X và giữ nguyên +Y.
    expect_near(rotate(from_yaw(0.0F), kZ), kZ, 0.0F);
    expect_near(rotate(from_yaw(kHalfPiF), kZ), kX, 1e-7F);
    expect_near(rotate(from_yaw(kHalfPiF), kX), -kZ, 1e-7F);
    expect_near(rotate(from_yaw(kHalfPiF), kY), kY, 1e-7F);
    expect_near(rotate(from_yaw(kPiF), kZ), -kZ, 1e-7F);
    expect_near(rotate(from_yaw(-kHalfPiF), kZ), -kX, 1e-7F);
}

TEST(Quat, AxisAngleFollowsRightHandRule) {
    expect_near(rotate(from_axis_angle(kZ, kHalfPiF), kX), kY, 1e-7F);
    expect_near(rotate(from_axis_angle(kX, kHalfPiF), kY), kZ, 1e-7F);
    expect_near(rotate(from_axis_angle(kY, kHalfPiF), kZ), kX, 1e-7F);
}

TEST(Quat, ProductAppliesRightOperandFirst) {
    const Quat yaw = from_yaw(0.7F);
    const Quat pitch = from_axis_angle(kX, -0.3F);
    const Vec3 v{0.3F, -1.2F, 2.5F};
    expect_near(rotate(yaw * pitch, v), rotate(yaw, rotate(pitch, v)), 1e-6F);
    expect_near(rotate(pitch * yaw, v), rotate(pitch, rotate(yaw, v)), 1e-6F);
}

TEST(Quat, ConjugateUndoesRotationAndKeepsLength) {
    Pcg32 rng(0x51, 1);
    for (u32 i = 0; i < 1'000; ++i) {
        const Quat q = random_unit_quat(rng);
        const Vec3 v{rng.next_f32() * 10.0F, rng.next_f32() * -10.0F, rng.next_f32()};
        expect_near(rotate(conjugate(q), rotate(q, v)), v, 1e-5F);
        EXPECT_NEAR(length(rotate(q, v)), length(v), 1e-5F);
        const Quat r = q * conjugate(q);
        EXPECT_NEAR(r.w, 1.0F, 1e-6F);
    }
}

TEST(Quat, NormalizeOrIdentity) {
    const Quat q = normalize_or_identity(Quat{0.0F, 3.0F, 0.0F, 4.0F});
    EXPECT_FLOAT_EQ(q.y, 0.6F);
    EXPECT_FLOAT_EQ(q.w, 0.8F);
    EXPECT_EQ(normalize_or_identity(Quat{0.0F, 0.0F, 0.0F, 0.0F}), Quat::identity());
    EXPECT_EQ(normalize_or_identity(Quat{3e38F, 3e38F, 0.0F, 0.0F}), Quat::identity());
}

TEST(Quat, SlerpHitsEndpointsAndTakesShortestPath) {
    const Quat a = from_yaw(0.2F);
    const Quat b = from_yaw(1.4F);
    EXPECT_LT(rotation_angle_between(slerp(a, b, 0.0F), a), 1e-6);
    EXPECT_LT(rotation_angle_between(slerp(a, b, 1.0F), b), 1e-6);
    EXPECT_LT(rotation_angle_between(slerp(a, b, 0.5F), from_yaw(0.8F)), 1e-6);
    // -b là cùng phép quay với b; slerp vẫn đi đường ngắn và cho cùng kết quả.
    EXPECT_LT(rotation_angle_between(slerp(a, -b, 0.25F), from_yaw(0.5F)), 1e-6);
    EXPECT_LT(rotation_angle_between(nlerp(a, -b, 0.5F), from_yaw(0.8F)), 1e-6);
}

// slerp dùng nlerp khi hai quaternion gần trùng. Đo độ lệch của nhánh đó so với slerp chính xác
// tính bằng f64, và độ chính xác của nhánh slerp thật ngay trên ngưỡng.
TEST(Quat, SlerpStaysOnConstantAngularVelocity) {
    for (const f64 total : {0.0632, 0.0633, 0.5, 2.5}) {
        const Quat a = Quat::identity();
        const Quat b = from_yaw(static_cast<f32>(total));
        f64 worst = 0.0;
        for (u32 step = 0; step <= 64; ++step) {
            const f64 t = step / 64.0;
            const Quat expected = from_yaw(static_cast<f32>(total * t));
            worst =
                std::max(worst, rotation_angle_between(slerp(a, b, static_cast<f32>(t)), expected));
        }
        EXPECT_LT(worst, 1.2e-6) << "góc " << total;
    }
}

}  // namespace
}  // namespace orion::math
