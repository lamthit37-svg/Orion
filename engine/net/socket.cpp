#include "engine/net/socket.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/net/address.hpp"
#include "engine/net/detail/native_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace orion::net {

Result<UdpSocket> UdpSocket::open(const Address& local) noexcept {
    const Result<std::intptr_t> handle = detail::open_udp(local);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return UdpSocket(*handle, local.family());
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)), family_(other.family_) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        if (handle_ != -1) {
            detail::close_socket(handle_);
        }
        handle_ = std::exchange(other.handle_, -1);
        family_ = other.family_;
    }
    return *this;
}

UdpSocket::~UdpSocket() {
    if (handle_ != -1) {
        detail::close_socket(handle_);
    }
}

Result<Address> UdpSocket::local_address() const noexcept {
    if (handle_ == -1) {
        return fail(ErrorCode::FailedPrecondition, "net: socket đã chuyển đi");
    }
    return detail::socket_address(handle_);
}

Result<void> UdpSocket::send(const Address& to,
                             const std::span<const std::byte> data) const noexcept {
    if (handle_ == -1) {
        return fail(ErrorCode::FailedPrecondition, "net: socket đã chuyển đi");
    }
    if (to.family() != family_) {
        return fail(ErrorCode::InvalidArgument, "net: đích khác họ địa chỉ với socket");
    }
    return detail::send_datagram(handle_, to, data);
}

Result<std::optional<Datagram>> UdpSocket::receive(
    const std::span<std::byte> buffer) const noexcept {
    if (handle_ == -1) {
        return fail(ErrorCode::FailedPrecondition, "net: socket đã chuyển đi");
    }
    return detail::receive_datagram(handle_, buffer);
}

Result<bool> UdpSocket::wait_readable(const core::Duration timeout) const noexcept {
    if (handle_ == -1) {
        return fail(ErrorCode::FailedPrecondition, "net: socket đã chuyển đi");
    }
    return detail::wait_readable(handle_, detail::poll_timeout_ms(timeout));
}

}  // namespace orion::net
