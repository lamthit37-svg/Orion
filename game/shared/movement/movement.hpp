#pragma once

// Di chuyển nhân vật, tất định (CLAUDE.md X.11): server mô phỏng, client dự đoán nhân vật của chính
// nó bằng cùng code rồi đối chiếu với server (ADR 0002 mục 5).
//
// Mỗi tick, ý định của client (protocol::MoveFrame, đã kẹp về khoảng hợp lệ) đổi vị trí ngang theo
// tốc độ chạy; nhảy, trọng lực và mặt đất đổi độ cao. Chỉ dùng + − × ÷ và căn bậc hai làm tròn đúng
// của IEEE 754 trên f64, không hợp nhất FMA (X.11), nên mọi toolchain cho cùng từng bit; golden
// replay ở tests/replay/ giữ điều đó.
//
// Chưa có physics (engine/physics, Jolt): mặt đất là một hàm độ cao do bên gọi đưa vào, không có
// tường hay va chạm giữa các nhân vật.

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/world_pos.hpp"
#include "game/shared/protocol/protocol.hpp"

#include <concepts>

namespace orion::movement {

// Biên của thế giới, trùng khoảng lượng tử hoá của WorldPosition trong movement.schema: vị trí mô
// phỏng luôn gửi được lên dây mà không bị kẹp.
inline constexpr f64 kWorldHalfExtent = 32'768.0;
inline constexpr f64 kWorldHalfHeight = 2'048.0;

// Tham số di chuyển của một loại nhân vật. Số mặc định là giả định thiết kế ban đầu, chưa qua thử
// chơi; về sau đọc từ data/ qua game/shared/defs.
struct Params {
    f64 run_speed = 6.0;        // m/s
    f64 jump_speed = 5.0;       // m/s theo phương đứng, lúc rời đất
    f64 gravity = 20.0;         // m/s²
    f64 max_fall_speed = 50.0;  // m/s
    // Mặt đất hạ thấp hơn chừng này trong một tick thì nhân vật rơi thay vì bám theo.
    f64 max_step_down = 0.5;  // m
};

// Ý định của một tick, đã chuẩn hoá: (x, z) theo trục thế giới, dài tối đa 1; yaw trong [0, 2π).
struct Intent {
    f64 x = 0.0;
    f64 z = 0.0;
    f64 yaw = 0.0;
    bool jump = false;

    friend bool operator==(const Intent&, const Intent&) = default;
};

// Mọi thứ cần để mô phỏng tick tiếp theo.
struct State {
    math::WorldPos position{};
    f64 yaw = 0.0;
    f64 vertical_speed = 0.0;  // m/s, dương là lên
    bool grounded = true;

    friend bool operator==(const State&, const State&) = default;
};

// Góc bất kỳ về [0, 2π); NaN và vô cực thành 0.
[[nodiscard]] f64 normalize_yaw(f64 yaw) noexcept;

// MoveFrame của protocol thành ý định: phần trăm thành [-1, 1], vector dài hơn 1 thì kẹp về 1,
// yaw về [0, 2π).
[[nodiscard]] Intent intent_from(const protocol::MoveFrame& frame) noexcept;

// Hai nửa của một tick; step ghép chúng với một hàm mặt đất.
[[nodiscard]] State move_horizontal(const State& state, const Intent& intent,
                                    const Params& params) noexcept;
// ground: độ cao mặt đất (m) dưới vị trí của state, hữu hạn.
[[nodiscard]] State move_vertical(const State& state, const Intent& intent, const Params& params,
                                  f64 ground) noexcept;

// ground(x, z) trả độ cao mặt đất tại (x, z); tham số mẫu nên lời gọi được inline, không có hàm ảo
// trong vòng lặp nóng (X.8).
template <class Ground>
    requires std::invocable<const Ground&, f64, f64>
[[nodiscard]] State step(const State& state, const Intent& intent, const Params& params,
                         const Ground& ground) noexcept {
    const State moved = move_horizontal(state, intent, params);
    return move_vertical(moved, intent, params, ground(moved.position.x, moved.position.z));
}

}  // namespace orion::movement
