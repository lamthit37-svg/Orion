#include "engine/core/assert.hpp"

#include "engine/core/types.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <source_location>
#include <string_view>

namespace orion::detail {

void assert_failed(const std::string_view macro, const std::string_view expression,
                   const std::string_view message, const std::source_location& where) noexcept {
    // Ghi thẳng ra stderr, không qua logger: logger có thể chính là thứ đang hỏng, và luồng ghi log
    // có thể không kịp chạy trước abort.
    std::array<char, 1024> line{};
    const auto result = std::format_to_n(line.data(), static_cast<isize>(line.size()),
                                         "{}:{}: {} thất bại: {}\n    {}\n", where.file_name(),
                                         where.line(), macro, expression, message);
    const auto length = std::min(static_cast<usize>(result.size), line.size());
    static_cast<void>(std::fwrite(line.data(), 1, length, stderr));
    static_cast<void>(std::fflush(stderr));
    std::abort();
}

}  // namespace orion::detail
