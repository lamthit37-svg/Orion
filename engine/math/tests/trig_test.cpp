#include "engine/math/trig.hpp"

#include "engine/core/types.hpp"
#include "engine/math/random.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/tests/support/ulp.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <ios>
#include <limits>
#include <vector>

namespace orion::math {
namespace {

constexpr f64 kInf = std::numeric_limits<f64>::infinity();
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();

using testing::ulp_distance;

[[nodiscard]] bool same_bits(const f64 a, const f64 b) {
    return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
}

[[nodiscard]] std::vector<f64> uniform(const f64 lo, const f64 hi, const u32 count,
                                       const u64 stream) {
    Pcg32 rng(0x5EED'0001ULL, stream);
    std::vector<f64> values;
    values.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        values.push_back(lo + ((hi - lo) * rng.next_f64()));
    }
    return values;
}

// Sai số lớn nhất, tính bằng ulp, so với hàm tham chiếu của thư viện chuẩn.
template <class Orion, class Reference>
[[nodiscard]] u64 max_ulp(const std::vector<f64>& inputs, Orion orion, Reference reference) {
    u64 worst = 0;
    for (const f64 x : inputs) {
        worst = std::max(worst, ulp_distance(orion(x), reference(x)));
    }
    return worst;
}

TEST(Trig, SinCosTanStayWithinTwoUlpOfStd) {
    const std::array ranges{std::array{-kPi / 4, kPi / 4}, std::array{-8 * kPi, 8 * kPi},
                            std::array{-kTrigReductionRange, kTrigReductionRange}};
    u64 stream = 0;
    for (const auto& [lo, hi] : ranges) {
        const std::vector<f64> xs = uniform(lo, hi, 20'000, ++stream);
        EXPECT_LE(max_ulp(
                      xs, [](f64 x) { return sin(x); }, [](f64 x) { return std::sin(x); }),
                  2U)
            << lo;
        EXPECT_LE(max_ulp(
                      xs, [](f64 x) { return cos(x); }, [](f64 x) { return std::cos(x); }),
                  2U)
            << lo;
        EXPECT_LE(max_ulp(
                      xs, [](f64 x) { return tan(x); }, [](f64 x) { return std::tan(x); }),
                  2U)
            << lo;
    }
}

TEST(Trig, InverseFunctionsStayWithinTwoUlpOfStd) {
    const std::vector<f64> unit = uniform(-1.0, 1.0, 20'000, 11);
    const std::vector<f64> wide = uniform(-50.0, 50.0, 20'000, 12);
    EXPECT_LE(max_ulp(wide, [](f64 x) { return atan(x); }, [](f64 x) { return std::atan(x); }), 2U);
    EXPECT_LE(max_ulp(unit, [](f64 x) { return asin(x); }, [](f64 x) { return std::asin(x); }), 2U);
    EXPECT_LE(max_ulp(unit, [](f64 x) { return acos(x); }, [](f64 x) { return std::acos(x); }), 2U);
    const std::vector<f64> ys = uniform(-100.0, 100.0, 20'000, 13);
    u64 worst = 0;
    for (usize i = 0; i < ys.size(); ++i) {
        worst = std::max(worst, ulp_distance(atan2(ys[i], wide[i]), std::atan2(ys[i], wide[i])));
    }
    EXPECT_LE(worst, 2U);
}

TEST(Trig, SinCosMatchesSeparateCalls) {
    for (const f64 x : uniform(-100.0, 100.0, 5'000, 21)) {
        const SinCos sc = sincos(x);
        EXPECT_TRUE(same_bits(sc.sin, sin(x))) << x;
        EXPECT_TRUE(same_bits(sc.cos, cos(x))) << x;
    }
}

TEST(Trig, FloatVersionsRoundTheDoubleResultOnce) {
    Pcg32 rng(0x5EED'0002ULL, 1);
    u64 worst = 0;
    for (u32 i = 0; i < 20'000; ++i) {
        const f32 x = ((rng.next_f32() * 2.0F) - 1.0F) * 100.0F;
        const f32 u = (rng.next_f32() * 2.0F) - 1.0F;
        const auto wide = static_cast<f64>(x);
        const auto unit = static_cast<f64>(u);
        worst = std::max(worst, ulp_distance(sin(x), static_cast<f32>(std::sin(wide))));
        worst = std::max(worst, ulp_distance(cos(x), static_cast<f32>(std::cos(wide))));
        worst = std::max(worst, ulp_distance(atan(x), static_cast<f32>(std::atan(wide))));
        worst = std::max(worst, ulp_distance(asin(u), static_cast<f32>(std::asin(unit))));
        worst = std::max(worst, ulp_distance(acos(u), static_cast<f32>(std::acos(unit))));
        worst =
            std::max(worst, ulp_distance(atan2(u, x), static_cast<f32>(std::atan2(unit, wide))));
        const SinCosF sc = sincos(x);
        EXPECT_EQ(std::bit_cast<u32>(sc.sin), std::bit_cast<u32>(sin(x)));
        EXPECT_EQ(std::bit_cast<u32>(sc.cos), std::bit_cast<u32>(cos(x)));
    }
    EXPECT_LE(worst, 1U);
}

TEST(Trig, ExactValuesAndSignedZero) {
    EXPECT_TRUE(same_bits(sin(0.0), 0.0));
    EXPECT_TRUE(same_bits(sin(-0.0), -0.0));
    EXPECT_TRUE(same_bits(tan(-0.0), -0.0));
    EXPECT_TRUE(same_bits(atan(-0.0), -0.0));
    EXPECT_TRUE(same_bits(asin(-0.0), -0.0));
    EXPECT_EQ(cos(0.0), 1.0);
    EXPECT_EQ(cos(-0.0), 1.0);
    EXPECT_EQ(atan(kInf), kHalfPi);
    EXPECT_EQ(atan(-kInf), -kHalfPi);
    EXPECT_EQ(asin(1.0), kHalfPi);
    EXPECT_EQ(asin(-1.0), -kHalfPi);
    EXPECT_TRUE(same_bits(acos(1.0), 0.0));
    EXPECT_EQ(acos(-1.0), kPi);
    EXPECT_EQ(acos(0.0), kHalfPi);
    // sin(π làm tròn) chính là phần π bị làm tròn mất: 1.2246467991473532e-16.
    EXPECT_LE(ulp_distance(sin(kPi), 1.2246467991473532e-16), 1U);
    EXPECT_EQ(std::bit_cast<u32>(sin(0.0F)), 0U);
    EXPECT_EQ(std::bit_cast<u32>(sin(-0.0F)), 0x8000'0000U);
}

TEST(Trig, NonFiniteInputsGiveNan) {
    for (const f64 x : {kInf, -kInf, kNan}) {
        EXPECT_TRUE(is_nan(sin(x)));
        EXPECT_TRUE(is_nan(cos(x)));
        EXPECT_TRUE(is_nan(tan(x)));
        EXPECT_TRUE(is_nan(sincos(x).sin));
        EXPECT_TRUE(is_nan(sincos(x).cos));
    }
    EXPECT_TRUE(is_nan(atan(kNan)));
    EXPECT_TRUE(is_nan(asin(1.0000000000000002)));
    EXPECT_TRUE(is_nan(acos(-1.0000000000000002)));
    EXPECT_TRUE(is_nan(asin(kNan)));
    EXPECT_TRUE(is_nan(atan2(kNan, 1.0)));
    EXPECT_TRUE(is_nan(atan2(1.0, kNan)));
}

// Mọi ca đặc biệt của atan2 theo C Annex F: ±0, vô cực, và dấu của kết quả.
TEST(Trig, Atan2FollowsAnnexFForZerosAndInfinities) {
    const std::array specials{0.0, -0.0, 1.5, -1.5, kInf, -kInf};
    for (const f64 y : specials) {
        for (const f64 x : specials) {
            const f64 expected = std::atan2(y, x);
            const bool exact_case = y == 0.0 || x == 0.0 || !is_finite(y) || !is_finite(x);
            if (exact_case) {
                EXPECT_TRUE(same_bits(atan2(y, x), expected)) << "y=" << y << " x=" << x;
            } else {
                EXPECT_LE(ulp_distance(atan2(y, x), expected), 1U) << "y=" << y << " x=" << x;
            }
        }
    }
}

// Ngoài kTrigReductionRange kết quả vẫn tất định, sai số tuyệt đối khoảng |x| · 4e-17.
TEST(Trig, HugeArgumentsStayBoundedAndClose) {
    for (const f64 x : uniform(2e6, 1e12, 5'000, 31)) {
        const SinCos sc = sincos(x);
        EXPECT_LE(abs(sc.sin), 1.0);
        EXPECT_LE(abs(sc.cos), 1.0);
        EXPECT_LE(abs(sc.sin - std::sin(x)), x * 1e-16) << x;
        EXPECT_LE(abs(sc.cos - std::cos(x)), x * 1e-16) << x;
    }
    EXPECT_LE(abs(sin(1e300)), 1.0);
    EXPECT_LE(abs(cos(-1e300)), 1.0);
}

// Băm từng bit kết quả (FNV-1a 64). NaN được quy về một mẫu vì payload không thuộc cam kết.
class BitHash {
public:
    void add(const f64 value) {
        add_bits(is_nan(value) ? 0x7FF8'0000'0000'0000ULL : std::bit_cast<u64>(value));
    }
    void add(const f32 value) {
        add_bits(is_nan(value) ? 0x7FC0'0000U : std::bit_cast<u32>(value));
    }
    [[nodiscard]] u64 value() const { return hash_; }

private:
    void add_bits(const u64 bits) {
        for (u32 shift = 0; shift < 64; shift += 8) {
            hash_ ^= (bits >> shift) & 0xFFU;
            hash_ *= 0x0000'0100'0000'01B3ULL;
        }
    }
    u64 hash_ = 0xCBF2'9CE4'8422'2325ULL;
};

// Đầu vào cố định: khoảng thường, cả khoảng giảm đối số chính xác, và mẫu bit bất kỳ (gồm
// subnormal, số rất lớn, vô cực, NaN).
[[nodiscard]] std::vector<f64> golden_inputs() {
    Pcg32 rng(0x0123'4567'89AB'CDEFULL, 7);
    std::vector<f64> values{
        0.0,  -0.0, kPi,   -kPi, kHalfPi, kTau, 1e-300, kTrigReductionRange, -kTrigReductionRange,
        1e22, kInf, -kInf, kNan};
    for (u32 i = 0; i < 4'096; ++i) {
        values.push_back((rng.next_f64() * 2.0 - 1.0) * 8.0 * kPi);
        values.push_back((rng.next_f64() * 2.0 - 1.0) * kTrigReductionRange);
    }
    for (u32 i = 0; i < 2'048; ++i) {
        values.push_back(std::bit_cast<f64>(rng.next_u64()));
    }
    return values;
}

struct GoldenHashes {
    u64 sin;
    u64 cos;
    u64 tan;
    u64 atan;
    u64 atan2;
    u64 asin_acos;
    u64 single;
};

[[nodiscard]] GoldenHashes compute_golden_hashes() {
    const std::vector<f64> xs = golden_inputs();
    std::array<BitHash, 7> hashes{};
    for (usize i = 0; i < xs.size(); ++i) {
        const f64 x = xs[i];
        const f64 unit = x / 8.0 / kPi;  // phần lớn nằm trong [-1, 1]
        hashes[0].add(sin(x));
        hashes[1].add(cos(x));
        hashes[2].add(tan(x));
        hashes[3].add(atan(x));
        hashes[4].add(atan2(x, xs[(i * 7 + 3) % xs.size()]));
        hashes[5].add(asin(unit));
        hashes[5].add(acos(unit));
        const auto xf = static_cast<f32>(x);
        const auto uf = static_cast<f32>(unit);
        hashes[6].add(sin(xf));
        hashes[6].add(cos(xf));
        hashes[6].add(tan(xf));
        hashes[6].add(atan(xf));
        hashes[6].add(atan2(uf, xf));
        hashes[6].add(asin(uf));
        hashes[6].add(acos(uf));
    }
    return {hashes[0].value(), hashes[1].value(), hashes[2].value(), hashes[3].value(),
            hashes[4].value(), hashes[5].value(), hashes[6].value()};
}

// Golden test của X.11: mọi toolchain phải cho đúng các hằng này. Hằng đổi nghĩa là kết quả đổi
// ở ít nhất một bit, nên golden replay (X.4) cũng đổi; chỉ cập nhật khi cố ý đổi thuật toán, và ghi
// lý do trong commit.
TEST(Trig, GoldenBitsAreIdenticalOnEveryToolchain) {
    const GoldenHashes actual = compute_golden_hashes();
    EXPECT_EQ(actual.sin, 0x6873'79C5'6883'B0DEULL) << std::hex << actual.sin;
    EXPECT_EQ(actual.cos, 0x0117'2ABF'B8B4'DA71ULL) << std::hex << actual.cos;
    EXPECT_EQ(actual.tan, 0xC3AB'67A5'1A06'CF3FULL) << std::hex << actual.tan;
    EXPECT_EQ(actual.atan, 0x98C5'C7CF'02BE'FC7BULL) << std::hex << actual.atan;
    EXPECT_EQ(actual.atan2, 0x29E5'10CC'F709'B6EDULL) << std::hex << actual.atan2;
    EXPECT_EQ(actual.asin_acos, 0x61BA'5E4F'D98B'1B40ULL) << std::hex << actual.asin_acos;
    EXPECT_EQ(actual.single, 0x8E55'3FA7'D51D'EF57ULL) << std::hex << actual.single;
}

}  // namespace
}  // namespace orion::math
