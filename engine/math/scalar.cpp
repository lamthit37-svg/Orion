#include "engine/math/scalar.hpp"

#include "engine/core/types.hpp"

#include <cmath>

// Chỉ các hàm cho kết quả duy nhất theo chuẩn mới được lấy từ <cmath>: sqrt làm tròn đúng theo
// IEEE 754; floor, ceil, trunc, round, fmod luôn biểu diễn được chính xác nên không có làm tròn.
// Golden test trong tests/trig_test.cpp so từng bit kết quả của chúng giữa các toolchain qua CI.

namespace orion::math {

f32 sqrt(const f32 value) noexcept {
    return std::sqrt(value);
}

f64 sqrt(const f64 value) noexcept {
    return std::sqrt(value);
}

f32 floor(const f32 value) noexcept {
    return std::floor(value);
}

f64 floor(const f64 value) noexcept {
    return std::floor(value);
}

f32 ceil(const f32 value) noexcept {
    return std::ceil(value);
}

f64 ceil(const f64 value) noexcept {
    return std::ceil(value);
}

f32 trunc(const f32 value) noexcept {
    return std::trunc(value);
}

f64 trunc(const f64 value) noexcept {
    return std::trunc(value);
}

f32 round(const f32 value) noexcept {
    return std::round(value);
}

f64 round(const f64 value) noexcept {
    return std::round(value);
}

f32 fmod(const f32 x, const f32 y) noexcept {
    return std::fmod(x, y);
}

f64 fmod(const f64 x, const f64 y) noexcept {
    return std::fmod(x, y);
}

}  // namespace orion::math
