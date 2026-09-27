#include "engine/math/trig.hpp"

#include "engine/core/types.hpp"
#include "engine/math/detail/double_double.hpp"
#include "engine/math/scalar.hpp"

#include <array>
#include <bit>
#include <limits>
#include <numbers>

// Cách tính: giảm đối số về |r| <= π/4 (sin, cos, tan) hoặc |z| <= 1/4 (atan) với phần dư giữ ở
// dạng hi + lo, rồi tính chuỗi Taylor bằng Horner. Các phép có thể mất chính xác (giảm đối số,
// 1 - r²/2, phép chia, 1 - x²) dùng biến đổi không sai số của detail/double_double.hpp, nên chỉ
// còn một lần làm tròn đáng kể ở cuối.
//
// Độ chính xác đo được ghi ở trig.hpp.

namespace orion::math {
namespace {

using detail::divide;
using detail::DoubleDouble;
using detail::fast_two_sum;
using detail::two_product;
using detail::two_sum;

// Dưới ngưỡng này, số hạng bậc ba của sin, tan, atan, asin nhỏ hơn nửa ulp nên kết quả làm tròn
// đúng là chính x (và 1 với cos). Trả thẳng x còn giữ được dấu của -0.
constexpr f64 kTinyArgument = 0x1.0p-27;

// π/2 = tổng bốn phần, lệch 7,4e-49. Ba phần đầu có tối đa 32 bit ý nghĩa nên với |k| <= 2^20 các
// tích k · phần đó đúng tuyệt đối. Các hằng trong tệp này tính bằng Python decimal từ 200 chữ số
// của π.
constexpr f64 kPio2Part1 = 1.5707963267341256;
constexpr f64 kPio2Part2 = 6.077100506303966e-11;
constexpr f64 kPio2Part3 = 2.0222662487111665e-21;
constexpr f64 kPio2Part4 = 8.4784276603689e-32;
constexpr f64 kTwoOverPi = 0.6366197723675814;
// Cộng rồi trừ 1.5 · 2^52 làm tròn một số |y| < 2^51 về số nguyên gần nhất (chế độ làm tròn mặc
// định của IEEE, không bao giờ đổi), và bit thấp của tổng chính là số nguyên đó. Không cần lời gọi
// floor() hay phép chuyển sang số nguyên trên đường tính.
constexpr f64 kRoundShifter = 0x1.8p52;

constexpr DoubleDouble kHalfPiDd{std::numbers::pi / 2, 6.123233995736766e-17};
constexpr DoubleDouble kPiDd{std::numbers::pi, 1.2246467991473532e-16};

// atan(k/16) với k = 4..16.
constexpr std::array<DoubleDouble, 13> kAtanSixteenths{{
    {0.24497866312686414, 1.0698755618734451e-17},
    {0.3028848683749714, -1.1010827903001369e-17},
    {0.35877067027057225, -2.4623815582638635e-17},
    {0.4124104415973873, -1.587652227770689e-17},
    {0.4636476090008061, 2.2698777452961687e-17},
    {0.5123894603107377, -2.5462781472855804e-17},
    {0.5585993153435624, -5.4556305485916264e-18},
    {0.6022873461349642, 2.950430737228402e-17},
    {0.6435011087932844, 1.5834785051444286e-17},
    {0.6823165548747481, 6.943223671560008e-18},
    {0.7188299996216245, -2.1478388444456983e-17},
    {0.7531512809621944, -2.4256934659182068e-17},
    {0.7853981633974483, 3.061616997868383e-17},
}};

// Hệ số Taylor làm tròn đúng từ số hữu tỉ: sin bậc 3..17, cos bậc 4..16, atan bậc 3..27. Trên miền
// dùng (|r| <= π/4; |z| <= 1/4 với đủ 13 hệ số atan, |z| <= 1/32 với 5 hệ số đầu), số hạng đầu tiên
// bị bỏ nhỏ hơn 2^-57 lần kết quả.
constexpr std::array<f64, 8> kSinCoefficients{
    -0.16666666666666666,   0.008333333333333333,   -0.0001984126984126984, 2.7557319223985893e-06,
    -2.505210838544172e-08, 1.6059043836821613e-10, -7.647163731819816e-13, 2.8114572543455206e-15,
};
constexpr std::array<f64, 7> kCosCoefficients{
    0.041666666666666664, -0.001388888888888889,   2.48015873015873e-05,  -2.755731922398589e-07,
    2.08767569878681e-09, -1.1470745597729725e-11, 4.779477332387385e-14,
};
constexpr std::array<f64, 13> kAtanCoefficients{
    -0.3333333333333333,   0.2,
    -0.14285714285714285,  0.1111111111111111,
    -0.09090909090909091,  0.07692307692307693,
    -0.06666666666666667,  0.058823529411764705,
    -0.05263157894736842,  0.047619047619047616,
    -0.043478260869565216, 0.04,
    -0.037037037037037035,
};
// Nhánh bảng của atan chỉ cần |z| <= 1/32: 5 hệ số đầu (bậc 3..11) là đủ.
constexpr usize kAtanTableTerms = 5;

// Horner trên Terms hệ số đầu, từ bậc cao nhất xuống. Thứ tự phép tính cố định để kết quả tất
// định.
template <usize Terms, usize N>
[[nodiscard]] f64 horner_prefix(const std::array<f64, N>& coefficients, const f64 x2) noexcept {
    static_assert(Terms >= 1 && Terms <= N);
    f64 sum = coefficients[Terms - 1];
    for (usize i = Terms - 1; i > 0; --i) {
        sum = coefficients[i - 1] + (x2 * sum);
    }
    return sum;
}

template <usize N>
[[nodiscard]] f64 horner(const std::array<f64, N>& coefficients, const f64 x2) noexcept {
    return horner_prefix<N>(coefficients, x2);
}

[[nodiscard]] f64 quiet_nan() noexcept {
    return std::numeric_limits<f64>::quiet_NaN();
}

// Kết quả cuối: một lần làm tròn duy nhất của head + tail.
[[nodiscard]] f64 round_sum(const DoubleDouble value) noexcept {
    return value.hi + value.lo;
}

// sin(hi + lo) với |hi + lo| <= π/4 xấp xỉ, trả head + tail chưa làm tròn:
// sin(hi + lo) ≈ sin(hi) + lo · cos(hi) ≈ hi + hi³ · S(hi²) + lo · (1 - hi²/2).
[[nodiscard]] DoubleDouble sin_kernel(const DoubleDouble r) noexcept {
    const f64 r2 = r.hi * r.hi;
    const f64 poly = r.hi * (r2 * horner(kSinCoefficients, r2));
    return {r.hi, poly + (r.lo - ((r.lo * r2) * 0.5))};
}

// cos(hi + lo) ≈ 1 - hi²/2 + hi⁴ · C(hi²) - lo · hi. hi² tính chính xác, và phần làm tròn của
// 1 - hi²/2 được giữ lại, nên sai số chỉ còn ở các số hạng nhỏ.
[[nodiscard]] DoubleDouble cos_kernel(const DoubleDouble r) noexcept {
    const DoubleDouble r2 = two_product(r.hi, r.hi);
    const f64 half = 0.5 * r2.hi;
    const f64 head = 1.0 - half;
    // head nằm trong [0.69, 1] nên 1 - head đúng tuyệt đối (Sterbenz); hiệu với half là đúng phần
    // làm tròn của head.
    const f64 head_error = (1.0 - head) - half;
    const f64 poly = (r2.hi * r2.hi) * horner(kCosCoefficients, r2.hi);
    return {head, head_error + ((poly - (0.5 * r2.lo)) - (r.hi * r.lo))};
}

struct Reduced {
    DoubleDouble r;
    u32 quadrant;
};

// x = k · π/2 + r với |r| <= π/4 xấp xỉ; quadrant = k mod 4. Tiền điều kiện: x hữu hạn.
[[nodiscard]] Reduced reduce(f64 x) noexcept {
    if (abs(x) > kTrigReductionRange) {
        x = fmod(x, kTau);
    }
    const f64 shifted = (x * kTwoOverPi) + kRoundShifter;
    const f64 k = shifted - kRoundShifter;
    // Tích k · phần 1..3 đúng tuyệt đối; x - k · phần 1 không làm tròn (Sterbenz). Các phần dư làm
    // tròn được gom lại bằng two_sum, nên r sai dưới 2^-100 tuyệt đối.
    const f64 a = x - (k * kPio2Part1);
    const DoubleDouble s1 = two_sum(a, -(k * kPio2Part2));
    const DoubleDouble s2 = two_sum(s1.hi, -(k * kPio2Part3));
    const f64 tail = (s1.lo + s2.lo) - (k * kPio2Part4);
    // |k| <= 2^20: hai bit thấp của shifted là k mod 4, kể cả khi k âm.
    return {two_sum(s2.hi, tail), static_cast<u32>(std::bit_cast<u64>(shifted) & 3U)};
}

// Giá trị của sin theo góc phần tư: sin(x) = ±sin(r) hoặc ±cos(r).
[[nodiscard]] f64 by_quadrant(const f64 sin_r, const f64 cos_r, const u32 quadrant) noexcept {
    const f64 value = (quadrant & 1U) == 0 ? sin_r : cos_r;
    return (quadrant & 2U) == 0 ? value : -value;
}

// sin(x) với x = quadrant · π/2 + r, chỉ tính một kernel. cos(x) là sin_of(r, quadrant + 1).
[[nodiscard]] f64 sin_of(const DoubleDouble r, const u32 quadrant) noexcept {
    const DoubleDouble value = (quadrant & 1U) == 0 ? sin_kernel(r) : cos_kernel(r);
    const f64 rounded = round_sum(value);
    return (quadrant & 2U) == 0 ? rounded : -rounded;
}

// atan(t) với t = hi + lo trong [0, 1], trả head + tail chưa làm tròn.
[[nodiscard]] DoubleDouble atan_unit(const DoubleDouble t) noexcept {
    if (t.hi <= 0.25) {
        // atan(hi + lo) ≈ atan(hi) + lo / (1 + hi²).
        const f64 t2 = t.hi * t.hi;
        return {t.hi, (t.hi * (t2 * horner(kAtanCoefficients, t2))) + (t.lo / (1.0 + t2))};
    }
    // atan(t) = atan(c) + atan(z) với c = k/16 gần t nhất và z = (t - c) / (1 + t·c), |z| <= 1/32.
    const f64 shifted = (t.hi * 16.0) + kRoundShifter;
    const f64 c = (shifted - kRoundShifter) * 0.0625;
    const auto index = static_cast<usize>(std::bit_cast<u64>(shifted) & 31U) - 4;  // k = 4..16
    const DoubleDouble tc = two_product(t.hi, c);
    const DoubleDouble one_plus = two_sum(1.0, tc.hi);
    const DoubleDouble denominator = fast_two_sum(one_plus.hi, one_plus.lo + (tc.lo + (t.lo * c)));
    // t.hi - c đúng tuyệt đối (Sterbenz, vì c >= 1/4 và |t.hi - c| <= 1/32).
    const DoubleDouble z = divide({t.hi - c, t.lo}, denominator);
    const f64 z2 = z.hi * z.hi;
    const f64 z_tail = (z.hi * (z2 * horner_prefix<kAtanTableTerms>(kAtanCoefficients, z2))) + z.lo;
    const DoubleDouble base = kAtanSixteenths[index];
    const DoubleDouble head = two_sum(base.hi, z.hi);
    return {head.hi, head.lo + (base.lo + z_tail)};
}

// π/2 - a và π - a, giữ phần thấp của hằng.
[[nodiscard]] DoubleDouble complement(const DoubleDouble a) noexcept {
    const DoubleDouble head = two_sum(kHalfPiDd.hi, -a.hi);
    return {head.hi, head.lo + (kHalfPiDd.lo - a.lo)};
}

[[nodiscard]] DoubleDouble supplement(const DoubleDouble a) noexcept {
    const DoubleDouble head = two_sum(kPiDd.hi, -a.hi);
    return {head.hi, head.lo + (kPiDd.lo - a.lo)};
}

// atan(small / large) với 0 <= small <= large, large hữu hạn và dương.
[[nodiscard]] DoubleDouble atan_ratio(f64 small, f64 large) noexcept {
    const f64 quotient = small / large;
    if (quotient < kTinyArgument) {
        // atan(q) làm tròn về q; phần dư của phép chia không đổi được kết quả.
        return {quotient, 0.0};
    }
    // Tỉ số >= 2^-27 nên hai số cách nhau không quá 2^27 lần. Nhân cả hai với cùng một lũy thừa của
    // 2 (chính xác) để two_product trong divide không tràn số và không rơi vào subnormal.
    if (large > 0x1.0p500) {
        small *= 0x1.0p-600;
        large *= 0x1.0p-600;
    } else if (large < 0x1.0p-500) {
        small *= 0x1.0p600;
        large *= 0x1.0p600;
    }
    return atan_unit(divide({small, 0.0}, {large, 0.0}));
}

// Góc trong [0, π/2] của điểm (x, y) với x, y >= 0 và ít nhất một số bằng 0 hoặc vô cực.
[[nodiscard]] DoubleDouble edge_angle(const f64 y, const f64 x) noexcept {
    if (y == 0.0 || (!is_finite(x) && is_finite(y))) {
        return {0.0, 0.0};
    }
    if (x == 0.0 || (!is_finite(y) && is_finite(x))) {
        return kHalfPiDd;
    }
    return kAtanSixteenths.back();  // cả hai vô cực: π/4
}

// sqrt(1 - a²) dạng hi + lo, với a trong [0, 1].
[[nodiscard]] DoubleDouble sqrt_one_minus_square(const f64 a) noexcept {
    const DoubleDouble square = two_product(a, a);
    const DoubleDouble difference = two_sum(1.0, -square.hi);
    const DoubleDouble w = fast_two_sum(difference.hi, difference.lo - square.lo);
    const f64 root = sqrt(w.hi);
    if (root == 0.0) {
        return {0.0, 0.0};
    }
    // w.hi - root² không làm tròn (Sterbenz); chia cho 2 · root là hiệu chỉnh Newton bậc một.
    const DoubleDouble root_square = two_product(root, root);
    return {root, (((w.hi - root_square.hi) - root_square.lo) + w.lo) / (2.0 * root)};
}

// atan(y / x) trong [0, π/2] với y, x >= 0 dạng hi + lo, không cùng bằng 0, và nằm trong [0, 1].
[[nodiscard]] DoubleDouble unit_angle(const DoubleDouble y, const DoubleDouble x) noexcept {
    if (y.hi <= x.hi) {
        return atan_unit(divide(y, x));
    }
    return complement(atan_unit(divide(x, y)));
}

}  // namespace

SinCos sincos(const f64 x) noexcept {
    if (!is_finite(x)) {
        return {quiet_nan(), quiet_nan()};
    }
    if (abs(x) < kTinyArgument) {
        return {x, 1.0};
    }
    const auto [r, quadrant] = reduce(x);
    const f64 sin_r = round_sum(sin_kernel(r));
    const f64 cos_r = round_sum(cos_kernel(r));
    return {by_quadrant(sin_r, cos_r, quadrant), by_quadrant(sin_r, cos_r, quadrant + 1)};
}

f64 sin(const f64 x) noexcept {
    if (!is_finite(x)) {
        return quiet_nan();
    }
    if (abs(x) < kTinyArgument) {
        return x;
    }
    const auto [r, quadrant] = reduce(x);
    return sin_of(r, quadrant);
}

f64 cos(const f64 x) noexcept {
    if (!is_finite(x)) {
        return quiet_nan();
    }
    if (abs(x) < kTinyArgument) {
        return 1.0;
    }
    const auto [r, quadrant] = reduce(x);
    return sin_of(r, quadrant + 1);
}

f64 tan(const f64 x) noexcept {
    if (!is_finite(x)) {
        return quiet_nan();
    }
    if (abs(x) < kTinyArgument) {
        return x;
    }
    const auto [r, quadrant] = reduce(x);
    const DoubleDouble sin_r = sin_kernel(r);
    const DoubleDouble cos_r = cos_kernel(r);
    const DoubleDouble s = fast_two_sum(sin_r.hi, sin_r.lo);
    const DoubleDouble c = fast_two_sum(cos_r.hi, cos_r.lo);
    // tan x = sin r / cos r ở góc phần tư chẵn, -cos r / sin r ở góc phần tư lẻ.
    return (quadrant & 1U) == 0 ? divide(s, c).hi : -divide(c, s).hi;
}

f64 atan(const f64 x) noexcept {
    const f64 a = abs(x);
    if (!(a >= kTinyArgument)) {
        return x;  // |x| nhỏ, hoặc NaN
    }
    const DoubleDouble angle = a <= 1.0 ? atan_unit({a, 0.0}) : complement(atan_ratio(1.0, a));
    return copy_sign(round_sum(angle), x);
}

f64 atan2(const f64 y, const f64 x) noexcept {
    if (is_nan(x) || is_nan(y)) {
        return quiet_nan();
    }
    const f64 ax = abs(x);
    const f64 ay = abs(y);
    DoubleDouble angle{};
    if (ax == 0.0 || ay == 0.0 || !is_finite(ax) || !is_finite(ay)) {
        angle = edge_angle(ay, ax);
    } else {
        angle = ay <= ax ? atan_ratio(ay, ax) : complement(atan_ratio(ax, ay));
    }
    if (sign_bit(x)) {
        angle = supplement(angle);
    }
    return copy_sign(round_sum(angle), y);
}

f64 asin(const f64 x) noexcept {
    const f64 a = abs(x);
    if (!(a >= kTinyArgument)) {
        return x;  // |x| nhỏ, hoặc NaN
    }
    if (a > 1.0) {
        return quiet_nan();
    }
    const DoubleDouble angle = unit_angle({a, 0.0}, sqrt_one_minus_square(a));
    return copy_sign(round_sum(angle), x);
}

f64 acos(const f64 x) noexcept {
    const f64 a = abs(x);
    if (!(a <= 1.0)) {
        return quiet_nan();  // |x| > 1, hoặc NaN
    }
    const DoubleDouble angle = unit_angle(sqrt_one_minus_square(a), {a, 0.0});
    return round_sum(x < 0.0 ? supplement(angle) : angle);
}

f32 sin(const f32 x) noexcept {
    return static_cast<f32>(sin(static_cast<f64>(x)));
}

f32 cos(const f32 x) noexcept {
    return static_cast<f32>(cos(static_cast<f64>(x)));
}

SinCosF sincos(const f32 x) noexcept {
    const SinCos sc = sincos(static_cast<f64>(x));
    return {static_cast<f32>(sc.sin), static_cast<f32>(sc.cos)};
}

f32 tan(const f32 x) noexcept {
    return static_cast<f32>(tan(static_cast<f64>(x)));
}

f32 atan(const f32 x) noexcept {
    return static_cast<f32>(atan(static_cast<f64>(x)));
}

f32 atan2(const f32 y, const f32 x) noexcept {
    return static_cast<f32>(atan2(static_cast<f64>(y), static_cast<f64>(x)));
}

f32 asin(const f32 x) noexcept {
    return static_cast<f32>(asin(static_cast<f64>(x)));
}

f32 acos(const f32 x) noexcept {
    return static_cast<f32>(acos(static_cast<f64>(x)));
}

}  // namespace orion::math
