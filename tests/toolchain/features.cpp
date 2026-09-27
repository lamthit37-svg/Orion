// Phần giao C++23 của năm toolchain (CLAUDE.md X.12).
//
// Code trong T0, T1 và game/shared chỉ được dùng tính năng có mặt ở đây. Muốn dùng thêm: thêm một
// static_assert theo feature-test macro (hoặc một đoạn biên dịch thử nếu tính năng không có
// macro), chờ CI xanh trên cả năm toolchain, rồi mới dùng. Giá trị so sánh là bản tối thiểu dự án
// cần, không phải giá trị đo được; số đo từng toolchain nằm ở NGHI-NGO-009 và NGHI-NGO-019.

// Header riêng của từng tính năng được include cùng <version>, để mỗi macro được kiểm cùng header
// mà code thật sẽ include. misc-include-cleaner không hiểu feature-test macro (macro có ở cả
// <version> lẫn header riêng): nó vừa báo header thừa vừa báo thiếu header cho cùng một macro, nên
// tắt riêng check này cho khối include và khối assert theo macro, không tắt cho phần biên dịch thử.
// NOLINTBEGIN(misc-include-cleaner)
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <compare>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <numbers>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <version>

namespace orion::toolchain {
namespace {

// So bằng hàm thay vì toán tử trực tiếp: khi macro bằng đúng giá trị tối thiểu, clang-tidy coi
// `a >= a` là biểu thức thừa. Macro vắng mặt vẫn là lỗi biên dịch vì định danh không tồn tại.
consteval bool at_least(const long measured, const long required) {
    return measured >= required;
}

// ---------------------------------------------------------------------------------------------
// Ngôn ngữ.
// ---------------------------------------------------------------------------------------------
static_assert(at_least(__cpp_concepts, 202002L), "concepts: ràng buộc template");
static_assert(at_least(__cpp_designated_initializers, 201707L),
              "designated initializers: struct cấu hình");
static_assert(at_least(__cpp_consteval, 201811L), "consteval: bảng hằng tính lúc biên dịch");
static_assert(at_least(__cpp_constinit, 201907L), "constinit: hằng toàn cục không khởi tạo động");
static_assert(at_least(__cpp_impl_three_way_comparison, 201907L),
              "operator<=> mặc định cho kiểu giá trị");
static_assert(at_least(__cpp_using_enum, 201907L), "using enum trong switch");
static_assert(at_least(__cpp_generic_lambdas, 201707L), "lambda có danh sách tham số template");

// ---------------------------------------------------------------------------------------------
// Thư viện chuẩn.
// ---------------------------------------------------------------------------------------------
static_assert(at_least(__cpp_lib_expected, 202211L), "std::expected kèm and_then, transform (X.5)");
static_assert(at_least(__cpp_lib_span, 202002L), "std::span cho vùng nhớ liền (X.7)");
static_assert(at_least(__cpp_lib_bit_cast, 201806L), "std::bit_cast thay ép kiểu qua union (X.3)");
static_assert(at_least(__cpp_lib_byteswap, 202110L),
              "std::byteswap cho thứ tự byte tường minh (X.10)");
static_assert(at_least(__cpp_lib_endian, 201907L), "std::endian");
static_assert(at_least(__cpp_lib_int_pow2, 202002L), "std::has_single_bit, std::bit_width");
static_assert(at_least(__cpp_lib_bitops, 201907L), "std::popcount, std::countl_zero");
static_assert(at_least(__cpp_lib_to_underlying, 202102L), "std::to_underlying cho enum class");
static_assert(at_least(__cpp_lib_unreachable, 202202L), "std::unreachable");
static_assert(at_least(__cpp_lib_source_location, 201907L),
              "std::source_location cho ORION_ASSERT");
static_assert(at_least(__cpp_lib_format, 201907L), "std::format cho log");
static_assert(at_least(__cpp_lib_math_constants, 201907L), "std::numbers::pi cho engine/math");
static_assert(at_least(__cpp_lib_concepts, 202002L), "<concepts>");
static_assert(at_least(__cpp_lib_ranges, 201911L), "<ranges>");
static_assert(at_least(__cpp_lib_three_way_comparison, 201907L), "<compare>");
static_assert(at_least(__cpp_lib_string_view, 201803L), "std::string_view");
static_assert(at_least(__cpp_lib_optional, 201606L), "std::optional");
static_assert(at_least(__cpp_lib_atomic_wait, 201907L),
              "std::atomic::wait, notify cho engine/jobs");
// NOLINTEND(misc-include-cleaner)

// ---------------------------------------------------------------------------------------------
// Biên dịch thử những gì không có macro riêng.
// ---------------------------------------------------------------------------------------------
enum class Probe : std::uint8_t {
    Odd = 1,
    Even = 2
};

// std::expected với kiểu lỗi là enum class, dùng trong ngữ cảnh constexpr.
constexpr std::expected<int, Probe> half(const int value) {
    if (value % 2 != 0) {
        return std::unexpected(Probe::Odd);
    }
    return value / 2;
}
static_assert(half(8).value_or(0) == 4);
static_assert(!half(3).has_value());
static_assert(half(3).error() == Probe::Odd);

// std::bit_cast và std::byteswap trong constexpr: nền của serializer có thứ tự byte tường minh.
static_assert(std::bit_cast<std::uint32_t>(1.0F) == 0x3F800000U);
static_assert(std::byteswap(std::uint32_t{0x11223344U}) == 0x44332211U);

// std::span dựng từ std::array trong constexpr.
constexpr int sum(const std::span<const int> values) {
    int total = 0;
    for (const int value : values) {
        total += value;
    }
    return total;
}
constexpr std::array<int, 3> kValues{1, 2, 3};
static_assert(sum(kValues) == 6);

// std::to_chars và std::from_chars cho số nguyên. Không dùng macro __cpp_lib_to_chars: libc++ của
// NDK r29 không định nghĩa nó vì bản số thực chưa đủ (NGHI-NGO-009), dù bản số nguyên có sẵn.
[[maybe_unused]] bool integer_charconv_round_trip(const std::uint32_t value) {
    std::array<char, 16> text{};
    const auto written = std::to_chars(text.data(), text.data() + text.size(), value);
    std::uint32_t parsed = 0;
    const auto read = std::from_chars(text.data(), written.ptr, parsed);
    return written.ec == std::errc{} && read.ec == std::errc{} && parsed == value;
}

}  // namespace
}  // namespace orion::toolchain
