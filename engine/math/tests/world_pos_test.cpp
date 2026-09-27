#include "engine/math/world_pos.hpp"

#include "engine/core/types.hpp"
#include "engine/math/vec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace orion::math {
namespace {

static_assert(WorldPos{1, 2, 3} - WorldPos{1, 1, 1} == DVec3{0, 1, 2});
static_assert(WorldPos{1, 2, 3} + DVec3{1, 1, 1} == WorldPos{2, 3, 4});
static_assert(WorldPos{1, 2, 3} - Vec3{1, 1, 1} == WorldPos{0, 1, 2});
static_assert(relative_to(WorldPos{10, 20, 30}, WorldPos{9, 18, 27}) == Vec3{1, 2, 3});
static_assert(distance_squared(WorldPos{0, 0, 0}, WorldPos{2, 3, 6}) == 49.0);

TEST(WorldPos, CompoundAssignment) {
    WorldPos p{100.0, 200.0, 300.0};
    p += DVec3{1.0, 2.0, 3.0};
    p += Vec3{0.5F, 0.5F, 0.5F};
    p -= DVec3{1.0, 1.0, 1.0};
    p -= Vec3{0.25F, 0.25F, 0.25F};
    EXPECT_EQ(p, (WorldPos{100.25, 201.25, 302.25}));
    EXPECT_EQ(distance(WorldPos{1.0, 1.0, 1.0}, WorldPos{4.0, 5.0, 1.0}), 5.0);
}

// ADR 0001: toạ độ tới hàng chục km, và xa hơn, vẫn giữ độ phân giải dưới micromet trong f64;
// vector tương đối với camera chỉ mất độ chính xác theo khoảng cách tới camera.
TEST(WorldPos, LargeCoordinatesKeepSubMillimetreDetail) {
    constexpr std::array kMagnitudes{20'000.0, 100'000.0, 10'000'000.0};
    for (const f64 magnitude : kMagnitudes) {
        const WorldPos camera{magnitude, -magnitude * 0.5, magnitude * 0.25};
        const WorldPos target = camera + DVec3{0.123'456, -0.000'789, 1.5};
        const Vec3 relative = relative_to(target, camera);
        // Sai số chỉ còn phép làm tròn f64 của target cộng làm tròn về f32: dưới 1e-8 m.
        EXPECT_NEAR(relative.x, 0.123'456F, 1e-8F) << magnitude;
        EXPECT_NEAR(relative.y, -0.000'789F, 1e-8F) << magnitude;
        EXPECT_NEAR(relative.z, 1.5F, 1e-8F) << magnitude;
        // Cùng phép tính bằng f32 tuyệt đối mất tới cỡ ulp của toạ độ: 2 mm ở 20 km.
        const f32 naive = static_cast<f32>(target.x) - static_cast<f32>(camera.x);
        EXPECT_GT(std::abs(naive - 0.123'456F), 1e-4F) << magnitude;
    }
}

TEST(WorldPos, DisplacementRoundTripIsExactForRepresentableSteps) {
    // Bước chân dạng k/1024 m biểu diễn chính xác ở f64 nên cộng rồi trừ trả đúng vị trí cũ, kể cả
    // ở 10 000 km.
    WorldPos p{10'000'000.0, 12.5, -7'000'000.0};
    const WorldPos start = p;
    const Vec3 step{0.25F, -0.125F, 1.0F / 1024.0F};
    for (u32 i = 0; i < 1'000; ++i) {
        p += step;
    }
    for (u32 i = 0; i < 1'000; ++i) {
        p -= step;
    }
    EXPECT_EQ(p, start);
    EXPECT_EQ(p - start, DVec3{});
}

}  // namespace
}  // namespace orion::math
