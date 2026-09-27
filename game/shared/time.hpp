#pragma once

// Thời gian của mô phỏng (ADR 0002): tick cố định 50 Hz. game/shared không đọc đồng hồ (CLAUDE.md
// X.11); thời gian đi vào dưới dạng số tick, và độ dài một tick là hằng ở đây.

#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <compare>

namespace orion::shared {

// Số tick của mô phỏng, đếm từ 0 khi zone khởi động; 64 bit nên không tràn trong đời một zone.
struct Tick {
    u64 value = 0;

    friend constexpr auto operator<=>(Tick, Tick) noexcept = default;
};

inline constexpr u32 kTickRate = 50;
static_assert(1'000 % kTickRate == 0, "một tick phải dài đúng một số mili giây");
inline constexpr core::Duration kTickDuration = core::Duration::milliseconds(1'000 / kTickRate);
// Độ dài một tick tính bằng giây, cho phép tính số thực của mô phỏng. Hằng tính lúc dịch nên mọi
// toolchain có cùng giá trị double.
inline constexpr f64 kTickSeconds = 1.0 / kTickRate;

}  // namespace orion::shared
