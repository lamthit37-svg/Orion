// Độ chính xác của trig so với bảng giá trị làm tròn đúng (tools/gen_math_reference.py, đối chiếu
// với mpmath). Kết quả tất định nên số liệu dưới đây giống nhau trên mọi toolchain.

#include "engine/core/types.hpp"
#include "engine/math/tests/support/ulp.hpp"
#include "engine/math/trig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace orion::math {
namespace {

struct ReferenceCase {
    std::string function;
    std::array<f64, 2> inputs{};  // atan2: y rồi x; hàm một biến chỉ dùng phần tử đầu
    f64 expected = 0.0;
};

[[nodiscard]] bool parse_bits(const std::string_view text, f64& value) {
    u64 bits = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), bits, 16);
    if (error != std::errc{} || end != text.data() + text.size() || text.size() != 16) {
        return false;
    }
    value = std::bit_cast<f64>(bits);
    return true;
}

// Mỗi dòng: tên hàm rồi hai hoặc ba trường bit hex 16 chữ số.
[[nodiscard]] std::vector<ReferenceCase> load_reference() {
    std::ifstream file(ORION_MATH_REFERENCE_FILE);
    std::vector<ReferenceCase> cases;
    std::string function;
    std::string first;
    std::string second;
    while (file >> function >> first >> second) {
        ReferenceCase entry{function};
        bool ok = parse_bits(first, entry.inputs[0]);
        if (function == "atan2") {
            std::string third;
            ok = ok && static_cast<bool>(file >> third) && parse_bits(second, entry.inputs[1]) &&
                 parse_bits(third, entry.expected);
        } else {
            ok = ok && parse_bits(second, entry.expected);
        }
        if (!ok) {
            ADD_FAILURE() << "dòng hỏng sau ca thứ " << cases.size();
            break;
        }
        cases.push_back(entry);
    }
    return cases;
}

[[nodiscard]] f64 evaluate(const ReferenceCase& entry) {
    const f64 x = entry.inputs[0];
    const std::string_view name = entry.function;
    if (name == "sin") {
        return sin(x);
    }
    if (name == "cos") {
        return cos(x);
    }
    if (name == "tan") {
        return tan(x);
    }
    if (name == "atan") {
        return atan(x);
    }
    if (name == "atan2") {
        return atan2(x, entry.inputs[1]);
    }
    if (name == "asin") {
        return asin(x);
    }
    if (name == "acos") {
        return acos(x);
    }
    ADD_FAILURE() << "hàm lạ trong bảng: " << name;
    return 0.0;
}

struct Tally {
    u32 total = 0;
    u32 correctly_rounded = 0;
};

// Mọi ca sai không quá 1 ulp so với giá trị làm tròn đúng (làm tròn trung thành), và mỗi hàm làm
// tròn đúng ít nhất 95% số ca.
TEST(TrigReference, FaithfullyRoundedOnEveryCase) {
    const std::vector<ReferenceCase> cases = load_reference();
    ASSERT_GE(cases.size(), 1'500U) << "không đọc được " << ORION_MATH_REFERENCE_FILE;
    std::map<std::string, Tally> tallies;
    for (const ReferenceCase& entry : cases) {
        const f64 actual = evaluate(entry);
        const u64 distance = testing::ulp_distance(actual, entry.expected);
        EXPECT_LE(distance, 1U) << entry.function << std::hexfloat << " x=" << entry.inputs[0]
                                << " y=" << entry.inputs[1] << " thật=" << entry.expected
                                << " được=" << actual;
        Tally& tally = tallies[entry.function];
        ++tally.total;
        tally.correctly_rounded += distance == 0 ? 1U : 0U;
    }
    EXPECT_EQ(tallies.size(), 7U);
    for (const auto& [function, tally] : tallies) {
        EXPECT_GE(tally.correctly_rounded * 100U, tally.total * 95U)
            << function << ": " << tally.correctly_rounded << "/" << tally.total;
    }
}

}  // namespace
}  // namespace orion::math
