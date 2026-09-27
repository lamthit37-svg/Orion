// Socket UDP trên macOS và iOS: socket(2) của Apple không nhận SOCK_NONBLOCK và SOCK_CLOEXEC, nên
// hai cờ đó được đặt bằng fcntl(2) ngay sau khi mở; phần còn lại dùng chung với Linux ở
// linux/posix_socket.hpp. Chưa build trên Apple (NGHI-NGO-032).

#include "engine/net/detail/native_socket.hpp"

#include "engine/core/error.hpp"
#include "engine/net/address.hpp"
#include "engine/net/linux/posix_socket.hpp"
#include "engine/net/socket.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace orion::net::detail {

Result<std::intptr_t> open_udp(const Address& local) noexcept {
    const int fd = ::socket(posix::family_of(local), SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return posix::errno_error(errno, "net: socket thất bại");
    }
    const int status = ::fcntl(fd, F_GETFL, 0);
    if (status < 0 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) != 0 ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        const int error = errno;
        static_cast<void>(::close(fd));
        return posix::errno_error(error, "net: không đặt được O_NONBLOCK hay FD_CLOEXEC");
    }
    return posix::configure_and_bind(fd, local);
}

void close_socket(const std::intptr_t handle) noexcept {
    posix::close_socket(handle);
}

Result<Address> socket_address(const std::intptr_t handle) noexcept {
    return posix::socket_address(handle);
}

Result<void> send_datagram(const std::intptr_t handle, const Address& to,
                           const std::span<const std::byte> data) noexcept {
    return posix::send_datagram(handle, to, data);
}

Result<std::optional<Datagram>> receive_datagram(const std::intptr_t handle,
                                                 const std::span<std::byte> buffer) noexcept {
    return posix::receive_datagram(handle, buffer);
}

Result<bool> wait_readable(const std::intptr_t handle, const int timeout_ms) noexcept {
    return posix::wait_readable(handle, timeout_ms);
}

}  // namespace orion::net::detail
