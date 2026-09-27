// Socket UDP trên Windows (Winsock 2). WSAStartup chạy đúng một lần cho cả tiến trình. Socket tạo
// bằng WSASocketW với WSA_FLAG_NO_HANDLE_INHERIT (tiến trình con không thừa kế, như SOCK_CLOEXEC)
// và đặt không chặn bằng ioctlsocket(FIONBIO). SIO_UDP_CONNRESET được tắt: khi bật (mặc định), gói
// ICMP "port unreachable" của một lần gửi trước làm recvfrom trả WSAECONNRESET, điều UDP không cần.
// Gói lớn hơn bộ đệm nhận cho WSAEMSGSIZE và bị bỏ.
//
// Con trỏ tới SOCKADDR_STORAGE đi qua void* thay vì reinterpret_cast (X.3); mọi trường được đọc ghi
// qua đúng kiểu sockaddr_in hay sockaddr_in6 bằng memcpy.

#include "engine/net/detail/native_socket.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/address.hpp"
#include "engine/net/socket.hpp"

#include <winsock2.h>
// mstcpip.h dùng các macro IOC_* của winsock2.h nên phải đứng sau nó.
#include <mstcpip.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>

namespace orion::net::detail {
namespace {

[[nodiscard]] std::unexpected<Error> socket_error(const int error,
                                                  const ErrorContext context) noexcept {
    switch (error) {
        case WSAEADDRINUSE:
            return fail(ErrorCode::AlreadyExists, context, error);
        case WSAEACCES:
            return fail(ErrorCode::PermissionDenied, context, error);
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT:
        case WSAEADDRNOTAVAIL:
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
            return fail(ErrorCode::Unavailable, context, error);
        case WSAEWOULDBLOCK:
        case WSAENOBUFS:
            return fail(ErrorCode::ResourceExhausted, context, error);
        case WSAEMSGSIZE:
            return fail(ErrorCode::InvalidArgument, context, error);
        default:
            return fail(ErrorCode::Io, context, error);
    }
}

[[nodiscard]] Result<void> ensure_winsock() noexcept {
    static const int status = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (status != 0) {
        return fail(ErrorCode::Unavailable, "net: WSAStartup thất bại", status);
    }
    return {};
}

[[nodiscard]] SOCKET native(const std::intptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

[[nodiscard]] int to_sockaddr(const Address& address, SOCKADDR_STORAGE& storage) noexcept {
    storage = {};
    const std::span<const u8> bytes = address.bytes();
    if (address.family() == AddressFamily::V4) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = htons(address.port());
        std::memcpy(&in.sin_addr, bytes.data(), bytes.size());
        std::memcpy(&storage, &in, sizeof(in));
        return static_cast<int>(sizeof(in));
    }
    sockaddr_in6 in6{};
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(address.port());
    std::memcpy(&in6.sin6_addr, bytes.data(), bytes.size());
    std::memcpy(&storage, &in6, sizeof(in6));
    return static_cast<int>(sizeof(in6));
}

[[nodiscard]] std::optional<Address> from_sockaddr(const SOCKADDR_STORAGE& storage,
                                                   const int size) noexcept {
    if (storage.ss_family == AF_INET && size >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in in{};
        std::memcpy(&in, &storage, sizeof(in));
        std::array<u8, 4> octets{};
        std::memcpy(octets.data(), &in.sin_addr, octets.size());
        return Address::v4(octets, ntohs(in.sin_port));
    }
    if (storage.ss_family == AF_INET6 && size >= static_cast<int>(sizeof(sockaddr_in6))) {
        sockaddr_in6 in6{};
        std::memcpy(&in6, &storage, sizeof(in6));
        std::array<u8, 16> bytes{};
        std::memcpy(bytes.data(), &in6.sin6_addr, bytes.size());
        return Address::v6(bytes, ntohs(in6.sin6_port));
    }
    return std::nullopt;
}

[[nodiscard]] sockaddr* as_sockaddr(SOCKADDR_STORAGE& storage) noexcept {
    void* const raw = &storage;
    return static_cast<sockaddr*>(raw);
}

[[nodiscard]] const sockaddr* as_sockaddr(const SOCKADDR_STORAGE& storage) noexcept {
    const void* const raw = &storage;
    return static_cast<const sockaddr*>(raw);
}

// Lỗi của một bước cấu hình: đóng socket rồi trả lỗi.
[[nodiscard]] std::unexpected<Error> close_with(const SOCKET udp,
                                                const ErrorContext context) noexcept {
    const int error = WSAGetLastError();
    static_cast<void>(closesocket(udp));
    return socket_error(error, context);
}

}  // namespace

Result<std::intptr_t> open_udp(const Address& local) noexcept {
    if (const Result<void> ready = ensure_winsock(); !ready) {
        return std::unexpected(ready.error());
    }
    const int family = local.family() == AddressFamily::V4 ? AF_INET : AF_INET6;
    const SOCKET udp = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (udp == INVALID_SOCKET) {
        return socket_error(WSAGetLastError(), "net: WSASocketW thất bại");
    }
    u_long non_blocking = 1;
    if (ioctlsocket(udp, FIONBIO, &non_blocking) != 0) {
        return close_with(udp, "net: không đặt được FIONBIO");
    }
    BOOL report_reset = FALSE;
    DWORD returned = 0;
    if (WSAIoctl(udp, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &returned,
                 nullptr, nullptr) != 0) {
        return close_with(udp, "net: không tắt được SIO_UDP_CONNRESET");
    }
    if (local.family() == AddressFamily::V6) {
        const DWORD on = 1;
        if (setsockopt(udp, IPPROTO_IPV6, IPV6_V6ONLY,
                       static_cast<const char*>(static_cast<const void*>(&on)), sizeof(on)) != 0) {
            return close_with(udp, "net: không bật được IPV6_V6ONLY");
        }
    }
    SOCKADDR_STORAGE storage{};
    const int size = to_sockaddr(local, storage);
    if (bind(udp, as_sockaddr(storage), size) != 0) {
        return close_with(udp, "net: bind thất bại");
    }
    return static_cast<std::intptr_t>(udp);
}

void close_socket(const std::intptr_t handle) noexcept {
    static_cast<void>(closesocket(native(handle)));
}

Result<Address> socket_address(const std::intptr_t handle) noexcept {
    SOCKADDR_STORAGE storage{};
    int size = sizeof(storage);
    if (getsockname(native(handle), as_sockaddr(storage), &size) != 0) {
        return socket_error(WSAGetLastError(), "net: getsockname thất bại");
    }
    const std::optional<Address> address = from_sockaddr(storage, size);
    if (!address) {
        return fail(ErrorCode::Internal, "net: họ địa chỉ lạ từ getsockname");
    }
    return *address;
}

Result<void> send_datagram(const std::intptr_t handle, const Address& to,
                           const std::span<const std::byte> data) noexcept {
    if (data.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return fail(ErrorCode::InvalidArgument, "net: gói quá lớn");
    }
    SOCKADDR_STORAGE storage{};
    const int size = to_sockaddr(to, storage);
    const void* const raw = data.data();
    if (sendto(native(handle), static_cast<const char*>(raw), static_cast<int>(data.size()), 0,
               as_sockaddr(storage), size) == SOCKET_ERROR) {
        return socket_error(WSAGetLastError(), "net: sendto thất bại");
    }
    return {};
}

Result<std::optional<Datagram>> receive_datagram(const std::intptr_t handle,
                                                 const std::span<std::byte> buffer) noexcept {
    const auto capacity = static_cast<int>(
        std::min(buffer.size(), static_cast<usize>(std::numeric_limits<int>::max())));
    void* const raw = buffer.data();
    while (true) {
        SOCKADDR_STORAGE storage{};
        int size = sizeof(storage);
        const int received = recvfrom(native(handle), static_cast<char*>(raw), capacity, 0,
                                      as_sockaddr(storage), &size);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                return std::optional<Datagram>{};
            }
            if (error == WSAEMSGSIZE) {
                return fail(ErrorCode::DataLoss, "net: gói lớn hơn bộ đệm nhận, đã bỏ",
                            static_cast<i64>(buffer.size()));
            }
            // Báo ICMP còn sót dù đã tắt SIO_UDP_CONNRESET: bỏ qua, nhận gói kế tiếp.
            if (error == WSAECONNRESET || error == WSAENETRESET) {
                continue;
            }
            return socket_error(error, "net: recvfrom thất bại");
        }
        const std::optional<Address> from = from_sockaddr(storage, size);
        if (!from) {
            return fail(ErrorCode::Internal, "net: họ địa chỉ lạ từ recvfrom");
        }
        return std::optional<Datagram>{Datagram{*from, static_cast<usize>(received)}};
    }
}

Result<bool> wait_readable(const std::intptr_t handle, const int timeout_ms) noexcept {
    // Gói UDP là dữ liệu thường (POLLRDNORM); POLLIN của Winsock gộp thêm POLLRDBAND (dữ liệu ưu
    // tiên), thứ UDP không có.
    WSAPOLLFD entry{};
    entry.fd = native(handle);
    entry.events = POLLRDNORM;
    const int ready = WSAPoll(&entry, 1, timeout_ms);
    if (ready == SOCKET_ERROR) {
        return socket_error(WSAGetLastError(), "net: WSAPoll thất bại");
    }
    return ready > 0;
}

}  // namespace orion::net::detail
