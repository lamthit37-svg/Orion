#include "engine/math/vec.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace orion::math {
namespace {

constexpr Vec3 kX{1.0F, 0.0F, 0.0F};
constexpr Vec3 kY{0.0F, 1.0F, 0.0F};
constexpr Vec3 kZ{0.0F, 0.0F, 1.0F};

// Phép tính không dùng sqrt chạy được lúc biên dịch.
static_assert(Vec3{1, 2, 3} + Vec3{4, 5, 6} == Vec3{5, 7, 9});
static_assert(Vec3{1, 2, 3} - Vec3{4, 5, 6} == Vec3{-3, -3, -3});
static_assert(-Vec3{1, -2, 3} == Vec3{-1, 2, -3});
static_assert(Vec3{1, 2, 3} * 2.0F == 2.0F * Vec3{1, 2, 3});
static_assert(Vec3{2, 4, 6} / 2.0F == Vec3{1, 2, 3});
static_assert(dot(Vec3{1, 2, 3}, Vec3{4, 5, 6}) == 32.0F);
static_assert(dot(Vec2{1, 2}, Vec2{3, 4}) == 11.0F);
static_assert(dot(Vec4{1, 2, 3, 4}, Vec4{1, 1, 1, 1}) == 10.0F);
static_assert(length_squared(DVec3{2, 3, 6}) == 49.0);
static_assert(distance_squared(Vec2{1, 1}, Vec2{4, 5}) == 25.0F);
static_assert(Vec3{} == Vec3{0, 0, 0});

// Hệ tay phải (ADR 0001): X × Y = Z, Y × Z = X, Z × X = Y.
static_assert(cross(kX, kY) == kZ);
static_assert(cross(kY, kZ) == kX);
static_assert(cross(kZ, kX) == kY);
static_assert(cross(kY, kX) == -kZ);

TEST(Vec, CompoundAssignmentMatchesBinaryOperators) {
    Vec3 v{1.0F, 2.0F, 3.0F};
    v += Vec3{1.0F, 1.0F, 1.0F};
    EXPECT_EQ(v, (Vec3{2.0F, 3.0F, 4.0F}));
    v -= Vec3{2.0F, 2.0F, 2.0F};
    EXPECT_EQ(v, (Vec3{0.0F, 1.0F, 2.0F}));
    v *= 3.0F;
    EXPECT_EQ(v, (Vec3{0.0F, 3.0F, 6.0F}));
    v /= 3.0F;
    EXPECT_EQ(v, (Vec3{0.0F, 1.0F, 2.0F}));

    Vec2 a{1.0F, 2.0F};
    a += Vec2{1.0F, 1.0F};
    a -= Vec2{0.5F, 0.5F};
    a *= 2.0F;
    a /= 4.0F;
    EXPECT_EQ(a, (Vec2{0.75F, 1.25F}));
    EXPECT_EQ(-a, (Vec2{-0.75F, -1.25F}));

    Vec4 b{1.0F, 2.0F, 3.0F, 4.0F};
    b += Vec4{1.0F, 1.0F, 1.0F, 1.0F};
    b -= Vec4{2.0F, 2.0F, 2.0F, 2.0F};
    b *= 2.0F;
    b /= 2.0F;
    EXPECT_EQ(b, (Vec4{0.0F, 1.0F, 2.0F, 3.0F}));
    EXPECT_EQ(-b + b, Vec4{});
    EXPECT_EQ(b * 0.5F, 0.5F * b);
}

TEST(Vec, LengthAndDistance) {
    EXPECT_EQ(length(Vec3{3.0F, 4.0F, 12.0F}), 13.0F);
    EXPECT_EQ(length(DVec3{2.0, 3.0, 6.0}), 7.0);
    EXPECT_EQ(length(Vec2{3.0F, 4.0F}), 5.0F);
    EXPECT_EQ(distance(Vec3{1.0F, 1.0F, 1.0F}, Vec3{4.0F, 5.0F, 1.0F}), 5.0F);
    EXPECT_EQ(length(Vec4{1.0F, 1.0F, 1.0F, 1.0F}), 2.0F);
}

TEST(Vec, NormalizeOrZeroHandlesDegenerateInput) {
    const Vec3 n = normalize_or_zero(Vec3{0.0F, 3.0F, 4.0F});
    EXPECT_FLOAT_EQ(n.y, 0.6F);
    EXPECT_FLOAT_EQ(n.z, 0.8F);
    EXPECT_NEAR(length(n), 1.0F, 1e-7F);
    EXPECT_EQ(normalize_or_zero(Vec3{}), Vec3{});
    // Bình phương độ dài bị làm tròn về 0 hoặc tràn thành vô cực: không trả NaN.
    constexpr f32 kTiny = std::numeric_limits<f32>::denorm_min();
    EXPECT_EQ(normalize_or_zero(Vec3{kTiny, 0.0F, 0.0F}), Vec3{});
    EXPECT_EQ(normalize_or_zero(Vec3{3e38F, 3e38F, 0.0F}), Vec3{});
    constexpr f32 kNanF = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_EQ(normalize_or_zero(Vec3{kNanF, 0.0F, 0.0F}), Vec3{});
    const DVec3 d = normalize_or_zero(DVec3{0.0, 0.0, -2.0});
    EXPECT_EQ(d, (DVec3{0.0, 0.0, -1.0}));
}

TEST(Vec, LerpMinMaxAbs) {
    EXPECT_EQ(lerp(Vec3{0.0F, 0.0F, 0.0F}, Vec3{2.0F, 4.0F, -8.0F}, 0.5F),
              (Vec3{1.0F, 2.0F, -4.0F}));
    EXPECT_EQ(lerp(Vec2{1.0F, 1.0F}, Vec2{3.0F, 5.0F}, 0.25F), (Vec2{1.5F, 2.0F}));
    EXPECT_EQ(min(Vec3{1.0F, 5.0F, -2.0F}, Vec3{2.0F, 3.0F, -4.0F}), (Vec3{1.0F, 3.0F, -4.0F}));
    EXPECT_EQ(max(Vec3{1.0F, 5.0F, -2.0F}, Vec3{2.0F, 3.0F, -4.0F}), (Vec3{2.0F, 5.0F, -2.0F}));
    EXPECT_EQ(abs(Vec3{-1.0F, 2.0F, -0.0F}), (Vec3{1.0F, 2.0F, 0.0F}));
}

TEST(Vec, DotIsEvaluatedLeftToRight) {
    // (1e8 + -1e8) + 1 = 1 khi cộng trái sang phải; thứ tự khác cho 0. Kết quả này là một phần của
    // cam kết tất định.
    EXPECT_EQ(dot(Vec3{1e8F, -1e8F, 1.0F}, Vec3{1.0F, 1.0F, 1.0F}), 1.0F);
}

}  // namespace
}  // namespace orion::math
