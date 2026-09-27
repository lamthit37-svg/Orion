#pragma once

// ORION_ASSERT và ORION_VERIFY (CLAUDE.md X.5).
//
// - ORION_ASSERT(điều kiện, "thông điệp {}", đối số...): lỗi lập trình, tức vi phạm tiền điều kiện
//   nội bộ. Bật ở mọi preset trừ ship; ở ship điều kiện vẫn được kiểm kiểu nhưng không được tính,
//   nên điều kiện không được có tác dụng phụ (clang-tidy bugprone-assert-side-effect bắt điều này).
// - ORION_VERIFY(...): bất biến mà vỡ thì hỏng bộ nhớ hoặc dữ liệu. Bật ở mọi bản, kể cả ship.
//
// Khi vỡ: in vị trí, biểu thức và thông điệp ra stderr, rồi abort để crash handler ghi minidump.
// Thông điệp chỉ được định dạng khi vỡ. Dữ liệu từ ngoài (mạng, file, DB, script, input người chơi,
// asset cooked) không bao giờ đi vào hai macro này: nó được kiểm rồi trả Error.

#include "engine/core/types.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

namespace orion::detail {

// Gọi khi một assert vỡ. Không trả về, không cấp phát, gọi được từ mọi luồng.
[[noreturn]] void assert_failed(std::string_view macro, std::string_view expression,
                                std::string_view message,
                                const std::source_location& where) noexcept;

// Định dạng thông điệp vào bộ đệm trên stack rồi gọi assert_failed. Thông điệp dài hơn bộ đệm bị
// cắt.
template <class... Args>
[[noreturn]] void assert_failed_format(const std::string_view macro,
                                       const std::string_view expression,
                                       const std::source_location& where,
                                       const std::format_string<Args...> format,
                                       Args&&... args) noexcept {
    std::array<char, 512> buffer{};
    const auto result = std::format_to_n(buffer.data(), static_cast<isize>(buffer.size()), format,
                                         std::forward<Args>(args)...);
    const auto length = std::min(static_cast<usize>(result.size), buffer.size());
    assert_failed(macro, expression, std::string_view(buffer.data(), length), where);
}

}  // namespace orion::detail

#define ORION_VERIFY(condition, ...)                                         \
    (static_cast<bool>(condition)                                            \
         ? static_cast<void>(0)                                              \
         : ::orion::detail::assert_failed_format("ORION_VERIFY", #condition, \
                                                 std::source_location::current(), __VA_ARGS__))

#if ORION_SHIP
#define ORION_ASSERT(condition, ...) static_cast<void>(sizeof(static_cast<bool>(condition)))
#else
#define ORION_ASSERT(condition, ...)                                         \
    (static_cast<bool>(condition)                                            \
         ? static_cast<void>(0)                                              \
         : ::orion::detail::assert_failed_format("ORION_ASSERT", #condition, \
                                                 std::source_location::current(), __VA_ARGS__))
#endif
