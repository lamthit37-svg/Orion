#include "engine/math/scalar.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace orion::math {
namespace {

constexpr f64 kInf = std::numeric_limits<f64>::infinity();
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();
constexpr f32 kInfF = std::numeric_limits<f32>::infinity();
constexpr f32 kNanF = std::numeric_limits<f32>::quiet_NaN();
constexpr f64 kDenormal = std::numeric_limits<f64>::denorm_min();

// Phân loại đọc bit nên chạy được lúc biên dịch.
static_assert(is_nan(kNan) && is_nan(-kNan) && is_nan(kNanF));
static_assert(!is_nan(kInf) && !is_nan(0.0) && !is_nan(kDenormal));
static_assert(is_finite(0.0) && is_finite(-kDenormal) && is_finite(1e308));
static_assert(!is_finite(kInf) && !is_finite(-kInfF) && !is_finite(kNan));
static_assert(sign_bit(-0.0) && sign_bit(-1.0F) && !sign_bit(0.0) && !sign_bit(kInf));
static_assert(abs(-2.5) == 2.5 && abs(-0.0F) == 0.0F && abs(kInfF) == kInfF);
static_assert(copy_sign(3.0, -0.0) == -3.0 && copy_sign(-3.0F, 1.0F) == 3.0F);
static_assert(min(1, 2) == 1 && max(1U, 2U) == 2U && clamp(7, 0, 5) == 5 && clamp(-1, 0, 5) == 0);
static_assert(lerp(2.0, 4.0, 0.5) == 3.0 && lerp(2.0F, 4.0F, 0.0F) == 2.0F);

TEST(Scalar, AbsClearsOnlyTheSignBit) {
    EXPECT_EQ(std::bit_cast<u64>(abs(-0.0)), 0U);
    EXPECT_EQ(std::bit_cast<u32>(abs(-0.0F)), 0U);
    // NaN giữ nguyên payload, chỉ mất bit dấu.
    const f64 nan_with_payload = std::bit_cast<f64>(0xFFF8'0000'0000'1234ULL);
    EXPECT_EQ(std::bit_cast<u64>(abs(nan_with_payload)), 0x7FF8'0000'0000'1234ULL);
    EXPECT_EQ(abs(-kDenormal), kDenormal);
}

TEST(Scalar, MinMaxReturnFirstArgumentOnNan) {
    EXPECT_EQ(min(1.0, 2.0), 1.0);
    EXPECT_EQ(max(1.0F, 2.0F), 2.0F);
    EXPECT_TRUE(is_nan(min(kNan, 1.0)));
    EXPECT_EQ(min(1.0, kNan), 1.0);
    EXPECT_EQ(clamp(0.5, 0.0, 1.0), 0.5);
    EXPECT_EQ(clamp(-kInf, 0.0, 1.0), 0.0);
}

TEST(Scalar, ExactOperationsMatchStandardLibrary) {
    constexpr std::array kValues{0.0,    -0.0, 0.5,   -0.5,  1.5,
                                 -1.5,   2.5,  -2.5,  1e300, -1e300,
                                 1e-310, 7.25, -7.75, 3e15,  4503599627370497.0};
    for (const f64 v : kValues) {
        EXPECT_EQ(std::bit_cast<u64>(floor(v)), std::bit_cast<u64>(std::floor(v))) << v;
        EXPECT_EQ(std::bit_cast<u64>(ceil(v)), std::bit_cast<u64>(std::ceil(v))) << v;
        EXPECT_EQ(std::bit_cast<u64>(trunc(v)), std::bit_cast<u64>(std::trunc(v))) << v;
        EXPECT_EQ(std::bit_cast<u64>(round(v)), std::bit_cast<u64>(std::round(v))) << v;
        EXPECT_EQ(std::bit_cast<u64>(sqrt(abs(v))), std::bit_cast<u64>(std::sqrt(std::abs(v))))
            << v;
        EXPECT_EQ(std::bit_cast<u64>(fmod(v, 0.75)), std::bit_cast<u64>(std::fmod(v, 0.75))) << v;
    }
    EXPECT_EQ(floor(-0.5F), -1.0F);
    EXPECT_EQ(std::bit_cast<u32>(ceil(-0.5F)), std::bit_cast<u32>(-0.0F));
    EXPECT_EQ(trunc(2.75F), 2.0F);
    EXPECT_EQ(round(2.5F), 3.0F);
    EXPECT_EQ(sqrt(16.0F), 4.0F);
    EXPECT_EQ(fmod(7.0F, 2.5F), 2.0F);
    EXPECT_TRUE(is_nan(sqrt(-1.0)));
    EXPECT_TRUE(is_nan(fmod(kInf, 1.0)));
}

TEST(Scalar, RoundGoesHalfwayAwayFromZero) {
    EXPECT_EQ(round(0.5), 1.0);
    EXPECT_EQ(round(-0.5), -1.0);
    EXPECT_EQ(round(1.5), 2.0);
    EXPECT_EQ(round(-2.5), -3.0);
    EXPECT_EQ(std::bit_cast<u64>(round(-0.25)), std::bit_cast<u64>(-0.0));
}

TEST(Scalar, FmodKeepsSignOfDividendAndIsExact) {
    EXPECT_EQ(fmod(-7.0, 2.0), -1.0);
    EXPECT_EQ(fmod(7.0, -2.0), 1.0);
    EXPECT_EQ(std::bit_cast<u64>(fmod(-4.0, 2.0)), std::bit_cast<u64>(-0.0));
    // 1e22 = 2^22 · 5^22 nên phần dư khi chia cho 3 là số nguyên nhỏ, biểu diễn chính xác.
    EXPECT_EQ(fmod(1e22, 3.0), 1.0);
}

}  // namespace
}  // namespace orion::math
