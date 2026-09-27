// Biến môi trường trên POSIX (Linux, Android): getenv. Bản của Apple ở apple/environment.cpp giống
// hệt; header hệ điều hành chỉ được include trong thư mục nền tảng, nên mỗi nơi một bản.

#include "engine/core/environment.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace orion::core {

std::optional<std::string> environment_variable(const std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string key(name);
    // getenv chỉ không an toàn khi luồng khác đổi môi trường cùng lúc; dự án không bao giờ đổi.
    const char* value = std::getenv(key.c_str());  // NOLINT(concurrency-mt-unsafe): xem trên.
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

}  // namespace orion::core
