#pragma once

// Phần riêng nền tảng của UdpSocket, cài ở linux/, apple/ và win/. Handle là file descriptor hay
// SOCKET đổi sang std::intptr_t; -1 là không có.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/address.hpp"
#include "engine/net/socket.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace orion::net::detail {

// Hạn chờ cho poll và WSAPoll, tính bằng mili giây kiểu int: hạn không dương thành 0 (không chờ),
// phần lẻ làm tròn lên để hạn dương không thành 0 (vòng chờ bận), hạn quá lớn chặn ở INT_MAX
// (khoảng 24,8 ngày).
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

[[nodiscard]] Result<std::intptr_t> open_udp(const Address& local) noexcept;
void close_socket(std::intptr_t handle) noexcept;
[[nodiscard]] Result<Address> socket_address(std::intptr_t handle) noexcept;
[[nodiscard]] Result<void> send_datagram(std::intptr_t handle, const Address& to,
                                         std::span<const std::byte> data) noexcept;
[[nodiscard]] Result<std::optional<Datagram>> receive_datagram(
    std::intptr_t handle, std::span<std::byte> buffer) noexcept;
// Như UdpSocket::wait_readable, với hạn đã đổi bằng poll_timeout_ms.
[[nodiscard]] Result<bool> wait_readable(std::intptr_t handle, int timeout_ms) noexcept;

}  // namespace orion::net::detail
