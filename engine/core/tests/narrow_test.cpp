#include "engine/core/narrow.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>

namespace orion {
namespace {

constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

// Mọi ca đều chạy được lúc biên dịch: nếu kiểm tra nào có UB, static_assert sẽ không biên dịch.
static_assert(try_narrow<u8>(255) == std::optional<u8>{255});
static_assert(!try_narrow<u8>(256).has_value());
static_assert(!try_narrow<u8>(-1).has_value());
static_assert(try_narrow<i8>(-128) == std::optional<i8>{-128});
static_assert(!try_narrow<i8>(-129).has_value());
static_assert(!try_narrow<u32>(i64{-1}).has_value());
static_assert(!try_narrow<i32>(u32{3'000'000'000U}).has_value());
static_assert(try_narrow<u64>(i64{7}) == std::optional<u64>{7});
static_assert(narrow<u16>(i32{65535}) == 65535);

TEST(Narrow, IntegerToInteger) {
    EXPECT_EQ(try_narrow<u16>(u64{65535}), std::optional<u16>{65535});
    EXPECT_FALSE(try_narrow<u16>(u64{65536}).has_value());
    EXPECT_FALSE(try_narrow<i64>(std::numeric_limits<u64>::max()).has_value());
    EXPECT_EQ(try_narrow<i64>(u64{1} << 62U), std::optional<i64>{i64{1} << 62});
    EXPECT_FALSE(try_narrow<u8>(std::numeric_limits<i64>::min()).has_value());
}

TEST(Narrow, FloatToInteger) {
    EXPECT_EQ(try_narrow<i32>(42.0), std::optional<i32>{42});
    EXPECT_EQ(try_narrow<u64>(-0.0), std::optional<u64>{0});
    EXPECT_FALSE(try_narrow<i32>(2.5).has_value());
    EXPECT_FALSE(try_narrow<i32>(1e20).has_value());
    EXPECT_FALSE(try_narrow<i32>(-1e20).has_value());
    EXPECT_FALSE(try_narrow<u32>(-1.0).has_value());
    EXPECT_FALSE(try_narrow<i64>(kNan).has_value());
    EXPECT_FALSE(try_narrow<i64>(kInf).has_value());
    // 2^63 là cận trên loại trừ của i64; 2^63 - 1024 là số double lớn nhất còn vừa.
    EXPECT_FALSE(try_narrow<i64>(9223372036854775808.0).has_value());
    EXPECT_EQ(try_narrow<i64>(9223372036854774784.0), std::optional<i64>{9223372036854774784});
    EXPECT_EQ(try_narrow<i64>(-9223372036854775808.0),
              std::optional<i64>{std::numeric_limits<i64>::min()});
}

TEST(Narrow, IntegerToFloat) {
    EXPECT_EQ(try_narrow<f32>(i32{16'777'216}), std::optional<f32>{16'777'216.0F});
    // 2^24 + 1 không biểu diễn đúng trong f32.
    EXPECT_FALSE(try_narrow<f32>(i32{16'777'217}).has_value());
    // u64 max làm tròn lên 2^64, ngoài khoảng u64: phải báo không vừa, không được UB.
    EXPECT_FALSE(try_narrow<f32>(std::numeric_limits<u64>::max()).has_value());
    EXPECT_FALSE(try_narrow<f64>(std::numeric_limits<u64>::max()).has_value());
    EXPECT_EQ(try_narrow<f64>(u64{1} << 53U), std::optional<f64>{9007199254740992.0});
}

TEST(Narrow, FloatToFloat) {
    EXPECT_EQ(try_narrow<f32>(0.5), std::optional<f32>{0.5F});
    EXPECT_FALSE(try_narrow<f32>(0.1).has_value());
    EXPECT_FALSE(try_narrow<f32>(1e300).has_value());
    EXPECT_FALSE(try_narrow<f32>(-1e300).has_value());
    EXPECT_EQ(try_narrow<f32>(kInf), std::optional<f32>{std::numeric_limits<f32>::infinity()});
    const auto nan = try_narrow<f32>(kNan);
    EXPECT_TRUE(nan.has_value() && detail::is_nan(*nan));
    EXPECT_EQ(try_narrow<f64>(0.1F), std::optional<f64>{static_cast<f64>(0.1F)});
}

TEST(NarrowDeathTest, NarrowAssertsAtCallSite) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
#if !ORION_SHIP
    const i32 big = 300;
    EXPECT_DEATH(static_cast<void>(narrow<u8>(big)), "narrow_test.cpp:[0-9]+: orion::narrow");
#endif
}

}  // namespace
}  // namespace orion
