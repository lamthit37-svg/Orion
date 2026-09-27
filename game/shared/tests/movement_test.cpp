// Di chuyển nhân vật (game/shared/movement): chuẩn hoá ý định, chạy, nhảy, trọng lực, mặt đất và
// biên của thế giới. Tất định giữa các toolchain được giữ bằng golden replay ở tests/replay/.

#include "game/shared/movement/movement.hpp"

#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/math/scalar.hpp"
#include "engine/net/channels.hpp"
#include "game/shared/protocol/codec.hpp"
#include "game/shared/protocol/protocol.hpp"
#include "game/shared/time.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace orion::movement {
namespace {

constexpr auto kFlat = [](const f64 /*x*/, const f64 /*z*/) noexcept {
    return 0.0;
};

[[nodiscard]] Intent run(const f64 x, const f64 z) {
    return Intent{.x = x, .z = z};
}

TEST(Movement, IntentsComeFromPercentAndAreClampedToUnitLength) {
    EXPECT_EQ(
        intent_from(protocol::MoveFrame{.move_x = 100, .move_z = 0, .yaw = 1.5, .jump = true}),
        (Intent{.x = 1.0, .z = 0.0, .yaw = 1.5, .jump = true}));
    const Intent half = intent_from(protocol::MoveFrame{.move_x = -50, .move_z = 25});
    EXPECT_EQ(half.x, -0.5);
    EXPECT_EQ(half.z, 0.25);
    // Chéo (100, −100) dài √2: kẹp về độ dài 1.
    const Intent diagonal = intent_from(protocol::MoveFrame{.move_x = 100, .move_z = -100});
    EXPECT_NEAR((diagonal.x * diagonal.x) + (diagonal.z * diagonal.z), 1.0, 1e-15);
    EXPECT_EQ(diagonal.x, -diagonal.z);
    // Khung dựng tay ngoài khoảng của schema cũng bị kẹp.
    EXPECT_EQ(intent_from(protocol::MoveFrame{.move_x = 127}).x, 1.0);
    EXPECT_EQ(intent_from(protocol::MoveFrame{.move_z = -128}).z, -1.0);
}

TEST(Movement, YawIsNormalizedIntoOneTurn) {
    EXPECT_EQ(normalize_yaw(0.0), 0.0);
    EXPECT_EQ(normalize_yaw(1.25), 1.25);
    EXPECT_EQ(normalize_yaw(math::kTau), 0.0);
    // Yaw lớn nhất của schema, 6,2832, vượt 2π một chút.
    EXPECT_NEAR(normalize_yaw(6.2832), 6.2832 - math::kTau, 1e-15);
    EXPECT_NEAR(normalize_yaw(-math::kHalfPi), 3 * math::kHalfPi, 1e-15);
    // Số âm rất nhỏ cộng 2π làm tròn thành đúng 2π: về 0. −0 thành +0.
    EXPECT_EQ(normalize_yaw(-1e-300), 0.0);
    EXPECT_FALSE(math::sign_bit(normalize_yaw(-0.0)));
    EXPECT_EQ(normalize_yaw(std::numeric_limits<f64>::quiet_NaN()), 0.0);
    EXPECT_EQ(normalize_yaw(-std::numeric_limits<f64>::infinity()), 0.0);
    for (i32 i = -300; i <= 300; ++i) {
        const f64 yaw = normalize_yaw(i * 0.37);
        EXPECT_GE(yaw, 0.0) << i;
        EXPECT_LT(yaw, math::kTau) << i;
    }
}

TEST(Movement, RunsAtRunSpeed) {
    const Params params;
    State state;
    for (u32 tick = 0; tick < shared::kTickRate; ++tick) {
        state = step(state, run(1.0, 0.0), params, kFlat);
    }
    // Một giây chạy thẳng là 6 m, trừ sai số làm tròn của 50 phép cộng.
    EXPECT_NEAR(state.position.x, 6.0, 1e-12);
    EXPECT_EQ(state.position.y, 0.0);
    EXPECT_EQ(state.position.z, 0.0);
    EXPECT_TRUE(state.grounded);
    // Yaw theo ý định, không theo hướng chạy.
    EXPECT_EQ(step(state, Intent{.x = 1.0, .yaw = 2.0}, params, kFlat).yaw, 2.0);
}

TEST(Movement, StopsAtTheEdgeOfTheWorld) {
    const Intent outward = intent_from(protocol::MoveFrame{.move_x = 100, .move_z = -100});
    State state{
        .position = {.x = kWorldHalfExtent - 0.05, .y = 0.0, .z = -kWorldHalfExtent + 0.05}};
    for (u32 tick = 0; tick < 10; ++tick) {
        state = step(state, outward, Params{}, kFlat);
    }
    EXPECT_EQ(state.position.x, kWorldHalfExtent);
    EXPECT_EQ(state.position.z, -kWorldHalfExtent);
    // Mặt đất ngoài khoảng độ cao cũng bị kẹp.
    const auto sky = [](const f64 /*x*/, const f64 /*z*/) {
        return 5'000.0;
    };
    EXPECT_EQ(step(State{}, Intent{}, Params{}, sky).position.y, kWorldHalfHeight);
}

// Biên của mô phỏng trùng khoảng của WorldPosition trong movement.schema: góc thế giới gửi lên dây
// rồi đọc lại không bị kẹp.
TEST(Movement, WorldBoundsFitTheWire) {
    for (const f64 sign : {1.0, -1.0}) {
        protocol::EntityUpdates updates;
        ASSERT_TRUE(
            updates.entities
                .push_back(protocol::EntityState{.id = protocol::ReplicatedId{1},
                                                 .position = {.x = sign * kWorldHalfExtent,
                                                              .y = -sign * kWorldHalfHeight,
                                                              .z = -sign * kWorldHalfExtent}})
                .has_value());
        std::array<std::byte, protocol::kMaxMessageSize> buffer{};
        const usize size = protocol::encode(updates, buffer).value_or(0);
        const auto decoded = protocol::decode_server_message(
            net::Channel::Unreliable, std::span<const std::byte>(buffer).first(size));
        ASSERT_TRUE(decoded.has_value());
        const auto& got = std::get<protocol::EntityUpdates>(*decoded).entities[0].position;
        EXPECT_EQ(got.x, sign * kWorldHalfExtent);
        EXPECT_EQ(got.y, -sign * kWorldHalfHeight);
        EXPECT_EQ(got.z, -sign * kWorldHalfExtent);
    }
}

struct Flight {
    f64 apex = 0.0;
    u32 ticks = 0;
};

// Nhảy từ mặt phẳng, giữ nút nhảy suốt chuyến bay: đỉnh và số tick trên không.
[[nodiscard]] Flight jump(const Params& params) {
    Flight flight;
    State state = step(State{}, Intent{.jump = true}, params, kFlat);
    while (!state.grounded && flight.ticks < 1'000) {
        flight.apex = math::max(flight.apex, state.position.y);
        ++flight.ticks;
        state = step(state, Intent{.jump = true}, params, kFlat);
    }
    EXPECT_EQ(state.position.y, 0.0);
    EXPECT_EQ(state.vertical_speed, 0.0);
    return flight;
}

TEST(Movement, JumpsFollowABallisticArc) {
    // Mỗi tick vận tốc giảm g·dt = 0,4 m/s rồi độ cao tăng vận tốc·dt: sau tick n độ cao là
    // 0,02·Σ(v₀ − 0,4k) = 0,02·v₀·n − 0,004·n(n + 1).
    // v₀ = 5: đỉnh ở tick 12, 0,576 m. Giữ nút nhảy trên không không nhảy thêm.
    const Flight standard = jump(Params{});
    EXPECT_NEAR(standard.apex, 0.576, 1e-12);
    // v₀ = 5,1: độ cao dương tới tick 24 (0,048 m) và âm ở tick 25: đáp đất đúng tick 25.
    const Flight high = jump(Params{.jump_speed = 5.1});
    EXPECT_EQ(high.ticks, 24U);
}

TEST(Movement, FollowsTheGroundAndFallsOffLedges) {
    const Params params;
    // Dốc 10%: bám đất suốt.
    const auto slope = [](const f64 x, const f64 /*z*/) {
        return 0.1 * x;
    };
    State state;
    for (u32 tick = 0; tick < 50; ++tick) {
        state = step(state, run(1.0, 0.0), params, slope);
        ASSERT_TRUE(state.grounded);
        ASSERT_EQ(state.position.y, 0.1 * state.position.x);
    }
    // Hụt 0,4 m, dưới max_step_down: vẫn bám đất.
    const auto small_drop = [](const f64 x, const f64 /*z*/) {
        return x < 0.1 ? 0.0 : -0.4;
    };
    state =
        step(step(State{}, run(1.0, 0.0), params, small_drop), run(1.0, 0.0), params, small_drop);
    EXPECT_TRUE(state.grounded);
    EXPECT_EQ(state.position.y, -0.4);
    // Vách hụt 3 m: rơi rồi đáp xuống mặt dưới.
    const auto cliff = [](const f64 x, const f64 /*z*/) {
        return x < 0.1 ? 0.0 : -3.0;
    };
    state = step(step(State{}, run(1.0, 0.0), params, cliff), run(1.0, 0.0), params, cliff);
    EXPECT_FALSE(state.grounded);
    EXPECT_LT(state.position.y, 0.0);
    for (u32 tick = 0; tick < 100 && !state.grounded; ++tick) {
        state = step(state, Intent{}, params, cliff);
    }
    EXPECT_TRUE(state.grounded);
    EXPECT_EQ(state.position.y, -3.0);
    EXPECT_EQ(state.vertical_speed, 0.0);
}

TEST(Movement, FallSpeedIsCapped) {
    const Params params;
    const auto abyss = [](const f64 /*x*/, const f64 /*z*/) {
        return -kWorldHalfHeight;
    };
    State state{.position = {.y = kWorldHalfHeight}, .grounded = false};
    f64 fastest = 0.0;
    for (u32 tick = 0; tick < 10'000 && !state.grounded; ++tick) {
        state = step(state, Intent{}, params, abyss);
        fastest = math::min(fastest, state.vertical_speed);
    }
    EXPECT_EQ(fastest, -params.max_fall_speed);
    EXPECT_TRUE(state.grounded);
    EXPECT_EQ(state.position.y, -kWorldHalfHeight);
}

// X.7: bước mô phỏng của cả một zone không cấp phát.
TEST(Movement, StepDoesNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    std::vector<State> crowd(5'500);
    const Params params;
    const core::testing::AllocationScope scope;
    for (State& state : crowd) {
        state = step(state, Intent{.x = 0.6, .z = -0.8, .jump = true}, params, kFlat);
    }
    EXPECT_EQ(scope.count(), 0U);
}

#if !ORION_SHIP
TEST(MovementDeathTest, GroundMustBeFinite) {
    const auto broken = [](const f64 /*x*/, const f64 /*z*/) {
        return std::numeric_limits<f64>::quiet_NaN();
    };
    EXPECT_DEATH(static_cast<void>(step(State{}, Intent{}, Params{}, broken)), "mặt đất");
}
#endif

}  // namespace
}  // namespace orion::movement
