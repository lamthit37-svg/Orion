#pragma once

// Vị trí tuyệt đối trong thế giới, tính bằng mét, ba số f64 (ADR 0001 mục 4).
//
// f32 ở toạ độ 20 000 m chỉ còn độ phân giải khoảng 2 mm, nên mô phỏng, game/shared và server lưu
// vị trí bằng WorldPos. Kiểu này chỉ có các phép của không gian affine: hai vị trí trừ nhau ra một
// độ lệch, vị trí cộng độ lệch ra vị trí; cộng hai vị trí với nhau không có nghĩa nên không có.

#include "engine/core/types.hpp"
#include "engine/math/vec.hpp"

namespace orion::math {

struct WorldPos {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;

    [[nodiscard]] friend constexpr DVec3 operator-(const WorldPos a, const WorldPos b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    [[nodiscard]] friend constexpr WorldPos operator+(const WorldPos p, const DVec3 d) noexcept {
        return {p.x + d.x, p.y + d.y, p.z + d.z};
    }
    [[nodiscard]] friend constexpr WorldPos operator-(const WorldPos p, const DVec3 d) noexcept {
        return {p.x - d.x, p.y - d.y, p.z - d.z};
    }
    // Độ lệch f32 (vận tốc · dt, offset cục bộ) được nâng lên f64 chính xác trước khi cộng.
    [[nodiscard]] friend constexpr WorldPos operator+(const WorldPos p, const Vec3 d) noexcept {
        return p + DVec3{d.x, d.y, d.z};
    }
    [[nodiscard]] friend constexpr WorldPos operator-(const WorldPos p, const Vec3 d) noexcept {
        return p - DVec3{d.x, d.y, d.z};
    }
    constexpr WorldPos& operator+=(const DVec3 d) noexcept { return *this = *this + d; }
    constexpr WorldPos& operator+=(const Vec3 d) noexcept { return *this = *this + d; }
    constexpr WorldPos& operator-=(const DVec3 d) noexcept { return *this = *this - d; }
    constexpr WorldPos& operator-=(const Vec3 d) noexcept { return *this = *this - d; }
    [[nodiscard]] friend constexpr bool operator==(const WorldPos&,
                                                   const WorldPos&) noexcept = default;
};

// Vector f32 từ origin tới p (ADR 0001 mục 5): trừ bằng f64 trước rồi mới làm tròn về f32, nên độ
// chính xác chỉ phụ thuộc khoảng cách tới origin chứ không phụ thuộc toạ độ tuyệt đối.
[[nodiscard]] constexpr Vec3 relative_to(const WorldPos p, const WorldPos origin) noexcept {
    const DVec3 d = p - origin;
    return {static_cast<f32>(d.x), static_cast<f32>(d.y), static_cast<f32>(d.z)};
}

[[nodiscard]] constexpr f64 distance_squared(const WorldPos a, const WorldPos b) noexcept {
    return length_squared(a - b);
}

[[nodiscard]] inline f64 distance(const WorldPos a, const WorldPos b) noexcept {
    return length(a - b);
}

}  // namespace orion::math
