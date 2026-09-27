#pragma once

// Chờ socket của libpq (PQsocket) sẵn sàng, có hạn: phần riêng nền tảng của server_db, cài ở linux/
// (poll) và win/ (WSAPoll). server chỉ build cho Linux và Windows (game/CMakeLists.txt).

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace orion::db::detail {

enum class Wait : u8 {
    Read,
    Write,
    ReadWrite,
};

// Hạn cho poll và WSAPoll bằng mili giây kiểu int: hạn không dương thành 0 (không chờ), phần lẻ làm
// tròn lên để hạn dương không thành 0 (vòng chờ bận), hạn quá lớn chặn ở INT_MAX.
[[nodiscard]] constexpr int poll_timeout_ms(const core::Duration timeout) noexcept {
    constexpr i64 kNanosecondsPerMillisecond = 1'000'000;
    const i64 ns = timeout.as_nanoseconds();
    if (ns <= 0) {
        return 0;
    }
    const i64 ms =
        (ns / kNanosecondsPerMillisecond) + (ns % kNanosecondsPerMillisecond != 0 ? 1 : 0);
    return static_cast<int>(std::min<i64>(ms, std::numeric_limits<int>::max()));
}

// Chờ `socket` sẵn sàng theo `wait`, hay có lỗi hoặc bị đóng (libpq tự đọc ra lỗi đó), tối đa
// `timeout_ms`. true khi sẵn sàng; false khi hết hạn, hay sớm hơn khi bị tín hiệu ngắt, nên bên gọi
// chờ trong vòng lặp có mốc hạn. Lỗi: Io.
[[nodiscard]] Result<bool> wait_socket(std::intptr_t socket, Wait wait, int timeout_ms) noexcept;

}  // namespace orion::db::detail
