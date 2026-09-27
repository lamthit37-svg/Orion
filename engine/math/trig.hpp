#pragma once

// Lượng giác tất định (CLAUDE.md X.11, ARCH §4.1).
//
// Thư viện C của mỗi nền tảng cài sin, cos, atan2 theo cách riêng, nên cùng một đầu vào có thể cho
// kết quả khác nhau ở bit cuối giữa MSVC, glibc và bionic, và golden replay (X.4) sẽ lệch. Các hàm
// ở đây chỉ dùng cộng, trừ, nhân, chia, sqrt, floor và fmod theo một thứ tự cố định, không FMA, nên
// cho cùng từng bit trên mọi toolchain. Bản f32 tính bằng f64 rồi làm tròn một lần.
//
// Độ chính xác đo ngày 2026-09-27 bằng mpmath 1.4.1 ở 160 bit, 200 000 điểm mỗi hàm (khoảng
// [-π/4, π/4], [-8π, 8π], cả khoảng giảm đối số, gần bội của π/2, gần ±1, độ lớn từ 2^-30 tới
// 2^30). Sai số lớn nhất so với giá trị đúng, tính bằng ulp của f64:
//
//   sin 0.748   cos 0.721   tan 0.853   atan 0.551   atan2 0.610   asin 0.545   acos 0.518
//
// Tức là trên mọi điểm đã đo, kết quả là một trong hai số f64 kề giá trị đúng (làm tròn trung
// thành). Tỉ lệ điểm được làm tròn đúng: trên 98% với sin, cos, tan; trên 99.9% với atan, atan2,
// asin, acos. tests/trig_reference_test.cpp giữ các mức này trên bảng làm tròn đúng của
// tools/gen_math_reference.py. NaN trả về là NaN, nhưng bit payload của nó không thuộc cam kết tất
// định.
//
// NGHI-NGO-028: bit kết quả trên arm64 (NDK, Apple clang) chưa đo; CI mới chạy test trên x64.

#include "engine/core/types.hpp"

namespace orion::math {

// Tới |x| = 2^20 · π/2, sin, cos, tan giảm đối số chính xác. Ngoài khoảng này x được đưa về
// [-2π, 2π] bằng fmod trên kTau; kết quả vẫn tất định, nhưng vì kTau lệch 2π khoảng 2.4e-16, nó
// lệch giá trị thật tới cỡ |x| · 4e-17.
inline constexpr f64 kTrigReductionRange = 1'647'099.3291652855;

struct SinCos {
    f64 sin;
    f64 cos;
};

struct SinCosF {
    f32 sin;
    f32 cos;
};

[[nodiscard]] f64 sin(f64 x) noexcept;
[[nodiscard]] f64 cos(f64 x) noexcept;
[[nodiscard]] SinCos sincos(f64 x) noexcept;
[[nodiscard]] f64 tan(f64 x) noexcept;
[[nodiscard]] f64 atan(f64 x) noexcept;
// Góc của điểm (x, y) trong [-π, π], theo quy ước của std::atan2 cho ±0 và vô cực.
[[nodiscard]] f64 atan2(f64 y, f64 x) noexcept;
// NaN khi |x| > 1.
[[nodiscard]] f64 asin(f64 x) noexcept;
[[nodiscard]] f64 acos(f64 x) noexcept;

[[nodiscard]] f32 sin(f32 x) noexcept;
[[nodiscard]] f32 cos(f32 x) noexcept;
[[nodiscard]] SinCosF sincos(f32 x) noexcept;
[[nodiscard]] f32 tan(f32 x) noexcept;
[[nodiscard]] f32 atan(f32 x) noexcept;
[[nodiscard]] f32 atan2(f32 y, f32 x) noexcept;
[[nodiscard]] f32 asin(f32 x) noexcept;
[[nodiscard]] f32 acos(f32 x) noexcept;

}  // namespace orion::math
