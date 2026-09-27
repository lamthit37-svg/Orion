#include "engine/core/assert.hpp"

#include <gtest/gtest.h>

#include <string>

namespace orion {
namespace {

// Hàm constexpr có assert: điều kiện đúng thì dùng được lúc biên dịch.
constexpr int checked_half(const int value) {
    ORION_ASSERT(value % 2 == 0, "giá trị {} phải chẵn", value);
    return value / 2;
}
static_assert(checked_half(8) == 4);

class AssertDeathTest : public ::testing::Test {
protected:
    // Kiểu "threadsafe" chạy lại tệp test trong tiến trình con, an toàn với sanitizer và luồng.
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST(Assert, PassingConditionsHaveNoEffect) {
    int evaluated = 0;
    ORION_VERIFY(++evaluated == 1, "VERIFY luôn tính điều kiện");
    EXPECT_EQ(evaluated, 1);
    ORION_ASSERT(evaluated == 1, "không in gì khi đúng");
    EXPECT_EQ(checked_half(10), 5);
}

TEST_F(AssertDeathTest, AssertReportsExpressionMessageAndLocation) {
    const int limit = 3;
    EXPECT_DEATH(
        ORION_ASSERT(limit > 5, "giới hạn {} quá nhỏ", limit),
        "assert_test.cpp:[0-9]+: ORION_ASSERT thất bại: limit > 5\n    giới hạn 3 quá nhỏ");
}

TEST_F(AssertDeathTest, VerifyReportsInEveryBuild) {
    const void* pointer = nullptr;
    EXPECT_DEATH(ORION_VERIFY(pointer != nullptr, "con trỏ rỗng"),
                 "ORION_VERIFY thất bại: pointer != nullptr\n    con trỏ rỗng");
}

TEST_F(AssertDeathTest, LongMessageIsTruncatedNotOverflowed) {
    const std::string huge(4096, 'x');
    EXPECT_DEATH(ORION_VERIFY(huge.empty(), "{}", huge), "ORION_VERIFY thất bại: huge.empty\\(\\)");
}

}  // namespace
}  // namespace orion
