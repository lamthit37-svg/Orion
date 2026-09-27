#include "game/shared/movement/movement.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"
#include "engine/math/scalar.hpp"
#include "game/shared/protocol/protocol.hpp"
#include "game/shared/time.hpp"

namespace orion::movement {

f64 normalize_yaw(const f64 yaw) noexcept {
    if (!math::is_finite(yaw)) {
        return 0.0;
    }
    // fmod đúng tuyệt đối, ra (−τ, τ) cùng dấu với yaw.
    const f64 wrapped = math::fmod(yaw, math::kTau);
    const f64 positive = wrapped < 0.0 ? wrapped + math::kTau : wrapped;
    // Số âm rất nhỏ cộng τ làm tròn thành đúng τ: đưa về 0 để giữ [0, τ). Cộng 0.0 biến −0 thành
    // +0, để cùng một hướng luôn có cùng từng bit.
    return positive < math::kTau ? positive + 0.0 : 0.0;
}

Intent intent_from(const protocol::MoveFrame& frame) noexcept {
    f64 x = static_cast<f64>(frame.move_x) / 100.0;
    f64 z = static_cast<f64>(frame.move_z) / 100.0;
    const f64 length_squared = (x * x) + (z * z);
    if (length_squared > 1.0) {
        const f64 length = math::sqrt(length_squared);
        x /= length;
        z /= length;
    }
    return Intent{.x = x, .z = z, .yaw = normalize_yaw(frame.yaw), .jump = frame.jump};
}

State move_horizontal(const State& state, const Intent& intent, const Params& params) noexcept {
    const f64 distance = params.run_speed * shared::kTickSeconds;
    State next = state;
    next.position.x =
        math::clamp(state.position.x + (intent.x * distance), -kWorldHalfExtent, kWorldHalfExtent);
    next.position.z =
        math::clamp(state.position.z + (intent.z * distance), -kWorldHalfExtent, kWorldHalfExtent);
    next.yaw = intent.yaw;
    return next;
}

State move_vertical(const State& state, const Intent& intent, const Params& params,
                    const f64 ground) noexcept {
    ORION_ASSERT(math::is_finite(ground), "độ cao mặt đất {} không hữu hạn", ground);
    const f64 floor = math::clamp(ground, -kWorldHalfHeight, kWorldHalfHeight);
    State next = state;
    if (next.grounded && intent.jump) {
        next.grounded = false;
        next.vertical_speed = params.jump_speed;
    } else if (next.grounded) {
        // Bám đất khi lên dốc, xuống dốc hay lên bậc; mặt đất hụt quá max_step_down thì rơi.
        if (floor >= next.position.y - params.max_step_down) {
            next.position.y = floor;
            return next;
        }
        next.grounded = false;
        next.vertical_speed = 0.0;
    }
    // Euler nửa ẩn: vận tốc trước, rồi vị trí theo vận tốc mới.
    next.vertical_speed = math::max(next.vertical_speed - (params.gravity * shared::kTickSeconds),
                                    -params.max_fall_speed);
    next.position.y += next.vertical_speed * shared::kTickSeconds;
    if (next.position.y <= floor) {
        next.position.y = floor;
        next.vertical_speed = 0.0;
        next.grounded = true;
    }
    next.position.y = math::clamp(next.position.y, -kWorldHalfHeight, kWorldHalfHeight);
    return next;
}

}  // namespace orion::movement
