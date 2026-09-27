// Biến môi trường (engine/core/environment.hpp).

#include "engine/core/environment.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace orion::core {
namespace {

// PATH có trên mọi máy chạy test: Linux, Windows, macOS.
TEST(Environment, ReadsAVariableThatExists) {
    const std::optional<std::string> path = environment_variable("PATH");
    EXPECT_TRUE(path.has_value() && !path->empty());
}

TEST(Environment, MissingVariablesAndInvalidNamesHaveNoValue) {
    EXPECT_FALSE(environment_variable("ORION_ENVIRONMENT_TEST_KHONG_CO").has_value());
    EXPECT_FALSE(environment_variable("").has_value());
    // Tên có NUL bị cắt ở NUL nếu đưa thẳng cho hệ điều hành, thành "PA": không bao giờ tra.
    EXPECT_FALSE(environment_variable(std::string_view("PATH\0X", 6)).has_value());
}

// Giá trị không phải ASCII phải ra đúng UTF-8; trên Windows nó đi qua UTF-16. ctest chạy test này
// thêm một lần riêng với ORION_ENVIRONMENT_TEST đã đặt (engine/core/CMakeLists.txt); ở lần chạy
// thường không có biến đó thì bỏ qua.
TEST(Environment, ValuesAreUtf8) {
    const std::optional<std::string> value = environment_variable("ORION_ENVIRONMENT_TEST");
    if (!value.has_value()) {
        GTEST_SKIP() << "ORION_ENVIRONMENT_TEST chỉ được đặt ở test engine/core:environment-utf8";
    }
    EXPECT_EQ(*value, "Hà Nội ☃");
}

}  // namespace
}  // namespace orion::core
