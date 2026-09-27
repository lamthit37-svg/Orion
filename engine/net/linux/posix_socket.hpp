#pragma once

// Phần chung của socket UDP trên POSIX (Linux, Android, Apple): nằm trong linux/ vì header hệ điều
// hành chỉ được include trong thư mục nền tảng (check_layers), và được linux/native_socket.cpp lẫn
// apple/native_socket.cpp include. Socket BSD của hai nơi giống nhau; khác nhau chỉ là cách mở
// socket không chặn.
//
// API socket nhận sockaddr*; con trỏ tới sockaddr_storage đi qua void* thay vì reinterpret_cast
// (X.3), và mọi trường được đọc ghi qua đúng kiểu sockaddr_in hay sockaddr_in6 bằng memcpy.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/address.hpp"
#include "engine/net/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>

namespace orion::net::detail::posix {

[[nodiscard]] inline std::unexpected<Error> errno_error(const int error,
                                                        const ErrorContext context) noexcept {
    switch (error) {
        case EADDRINUSE:
            return fail(ErrorCode::AlreadyExists, context, error);
        case EACCES:
        case EPERM:
            return fail(ErrorCode::PermissionDenied, context, error);
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT:
        case EADDRNOTAVAIL:
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ECONNREFUSED:
            return fail(ErrorCode::Unavailable, context, error);
        case EAGAIN:
        case ENOBUFS:
        case ENOMEM:
            return fail(ErrorCode::ResourceExhausted, context, error);
        case EMSGSIZE:
            return fail(ErrorCode::InvalidArgument, context, error);
        default:
            return fail(ErrorCode::Io, context, error);
    }
}

// Không có gói để nhận, hay bộ đệm gửi đầy. EAGAIN và EWOULDBLOCK trùng nhau trên Linux và Apple.
[[nodiscard]] constexpr bool would_block(const int error) noexcept {
#if EAGAIN == EWOULDBLOCK
    return error == EAGAIN;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

// Gọi lại khi bị tín hiệu ngắt (EINTR).
template <class Call>
[[nodiscard]] auto retry_interrupted(const Call& call) noexcept {
    auto result = call();
    while (result < 0 && errno == EINTR) {
        result = call();
    }
    return result;
}

[[nodiscard]] inline int descriptor(const std::intptr_t handle) noexcept {
    return static_cast<int>(handle);
}

[[nodiscard]] inline int family_of(const Address& address) noexcept {
    return address.family() == AddressFamily::V4 ? AF_INET : AF_INET6;
}

// Ghi `address` vào `storage`; trả độ dài của dạng sockaddr tương ứng.
[[nodiscard]] inline socklen_t to_sockaddr(const Address& address,
                                           sockaddr_storage& storage) noexcept {
    storage = {};
    const std::span<const u8> bytes = address.bytes();
    if (address.family() == AddressFamily::V4) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = htons(address.port());
        std::memcpy(&in.sin_addr, bytes.data(), bytes.size());
        std::memcpy(&storage, &in, sizeof(in));
        return sizeof(in);
    }
    sockaddr_in6 in6{};
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(address.port());
    std::memcpy(&in6.sin6_addr, bytes.data(), bytes.size());
    std::memcpy(&storage, &in6, sizeof(in6));
    return sizeof(in6);
}

// Địa chỉ trong `storage`, hay nullopt khi họ địa chỉ lạ.
[[nodiscard]] inline std::optional<Address> from_sockaddr(const sockaddr_storage& storage,
                                                          const socklen_t size) noexcept {
    if (storage.ss_family == AF_INET && size >= sizeof(sockaddr_in)) {
        sockaddr_in in{};
        std::memcpy(&in, &storage, sizeof(in));
        std::array<u8, 4> octets{};
        std::memcpy(octets.data(), &in.sin_addr, octets.size());
        return Address::v4(octets, ntohs(in.sin_port));
    }
    if (storage.ss_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
        sockaddr_in6 in6{};
        std::memcpy(&in6, &storage, sizeof(in6));
        std::array<u8, 16> bytes{};
        std::memcpy(bytes.data(), &in6.sin6_addr, bytes.size());
        return Address::v6(bytes, ntohs(in6.sin6_port));
    }
    return std::nullopt;
}

[[nodiscard]] inline sockaddr* as_sockaddr(sockaddr_storage& storage) noexcept {
    void* const raw = &storage;
    return static_cast<sockaddr*>(raw);
}

// Bật IPV6_V6ONLY cho socket IPv6 rồi bind; đóng socket khi lỗi.
[[nodiscard]] inline Result<std::intptr_t> configure_and_bind(const int fd,
                                                              const Address& local) noexcept {
    if (local.family() == AddressFamily::V6) {
        const int on = 1;
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) != 0) {
            const int error = errno;
            static_cast<void>(::close(fd));
            return errno_error(error, "net: không bật được IPV6_V6ONLY");
        }
    }
    sockaddr_storage storage{};
    const socklen_t size = to_sockaddr(local, storage);
    if (::bind(fd, as_sockaddr(storage), size) != 0) {
        const int error = errno;
        static_cast<void>(::close(fd));
        return errno_error(error, "net: bind thất bại");
    }
    return std::intptr_t{fd};
}

inline void close_socket(const std::intptr_t handle) noexcept {
    // Không gọi lại khi close trả EINTR: Linux đã giải phóng descriptor (close(2)), và gọi lại có
    // thể đóng nhầm descriptor mà luồng khác vừa mở; ở nơi khác, tệ nhất là rò một descriptor.
    static_cast<void>(::close(descriptor(handle)));
}

[[nodiscard]] inline Result<Address> socket_address(const std::intptr_t handle) noexcept {
    sockaddr_storage storage{};
    socklen_t size = sizeof(storage);
    if (::getsockname(descriptor(handle), as_sockaddr(storage), &size) != 0) {
        return errno_error(errno, "net: getsockname thất bại");
    }
    const std::optional<Address> address = from_sockaddr(storage, size);
    if (!address) {
        return fail(ErrorCode::Internal, "net: họ địa chỉ lạ từ getsockname");
    }
    return *address;
}

[[nodiscard]] inline Result<void> send_datagram(const std::intptr_t handle, const Address& to,
                                                const std::span<const std::byte> data) noexcept {
    sockaddr_storage storage{};
    const socklen_t size = to_sockaddr(to, storage);
    const ssize_t sent = retry_interrupted([&] {
        return ::sendto(descriptor(handle), data.data(), data.size(), 0, as_sockaddr(storage),
                        size);
    });
    if (sent < 0) {
        return errno_error(would_block(errno) ? EAGAIN : errno, "net: sendto thất bại");
    }
    return {};
}

[[nodiscard]] inline Result<std::optional<Datagram>> receive_datagram(
    const std::intptr_t handle, const std::span<std::byte> buffer) noexcept {
    sockaddr_storage storage{};
    iovec vector{};
    vector.iov_base = buffer.data();
    vector.iov_len = buffer.size();
    msghdr message{};
    message.msg_name = &storage;
    message.msg_namelen = sizeof(storage);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    const ssize_t received =
        retry_interrupted([&] { return ::recvmsg(descriptor(handle), &message, 0); });
    if (received < 0) {
        if (would_block(errno)) {
            return std::optional<Datagram>{};
        }
        return errno_error(errno, "net: recvmsg thất bại");
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        return fail(ErrorCode::DataLoss, "net: gói lớn hơn bộ đệm nhận, đã bỏ",
                    static_cast<i64>(buffer.size()));
    }
    const std::optional<Address> from = from_sockaddr(storage, message.msg_namelen);
    if (!from) {
        return fail(ErrorCode::Internal, "net: họ địa chỉ lạ từ recvmsg");
    }
    return std::optional<Datagram>{Datagram{*from, static_cast<usize>(received)}};
}

// Bị tín hiệu ngắt thì trả false (như hết hạn) thay vì chờ lại cả hạn: bên gọi vốn chờ trong vòng
// lặp, và chờ lại có thể kéo dài quá hạn nó đặt.
[[nodiscard]] inline Result<bool> wait_readable(const std::intptr_t handle,
                                                const int timeout_ms) noexcept {
    pollfd entry{};
    entry.fd = descriptor(handle);
    entry.events = POLLIN;
    const int ready = ::poll(&entry, 1, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return false;
        }
        return errno_error(errno, "net: poll thất bại");
    }
    return ready > 0;
}

}  // namespace orion::net::detail::posix
