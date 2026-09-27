// Hằng thời gian của mô phỏng (ADR 0002).

#include "game/shared/time.hpp"

#include "engine/core/time.hpp"

#include <gtest/gtest.h>

namespace orion::shared {
namespace {

TEST(Time, OneTickIsTwentyMilliseconds) {
    EXPECT_EQ(kTickRate, 50U);
    EXPECT_EQ(kTickDuration, core::Duration::milliseconds(20));
    // 1/50 làm tròn đúng là chính số double gần 0,02 nhất, và 50 tick cộng lại đúng một giây.
    EXPECT_EQ(kTickSeconds, 0.02);
    EXPECT_EQ(kTickSeconds * kTickRate, 1.0);
}

TEST(Time, TicksCompareByValue) {
    EXPECT_LT(Tick{1}, Tick{2});
    EXPECT_EQ(Tick{7}, Tick{7});
    EXPECT_EQ(Tick{}.value, 0U);
}

}  // namespace
}  // namespace orion::shared
