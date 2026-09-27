#pragma once

// Socket UDP không chặn (ARCH §4.5): gửi và nhận từng gói, send và receive trả ngay. Vòng lặp của
// client gọi receive mỗi frame tới khi hết gói; luồng nào cần chờ gói thì chờ bằng wait_readable
// với hạn, để còn xem cờ dừng giữa các lần chờ. Lỗi của hệ điều hành đổi sang ErrorCode. Cài đặt
// riêng từng nền tảng ở linux/ (cả Android), apple/ và win/.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/address.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace orion::net {

// Một gói đã nhận: nơi gửi và số byte ở đầu bộ đệm.
struct Datagram {
    Address from;
    usize size = 0;
};

class UdpSocket {
public:
    // Mở socket UDP không chặn và bind vào `local`; cổng 0 để hệ điều hành chọn. Socket IPv6 chỉ
    // nhận IPv6 (IPV6_V6ONLY bật ở mọi hệ điều hành, vì mặc định khác nhau). Lỗi: AlreadyExists
    // (cổng đang được dùng), PermissionDenied, Unavailable (hệ điều hành không có họ địa chỉ này
    // hay địa chỉ không thuộc máy), Io.
    [[nodiscard]] static Result<UdpSocket> open(const Address& local) noexcept;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    ~UdpSocket();

    [[nodiscard]] AddressFamily family() const noexcept { return family_; }
    // Địa chỉ đã bind, với cổng thật khi mở bằng cổng 0.
    [[nodiscard]] Result<Address> local_address() const noexcept;

    // Gửi một gói. Lỗi: InvalidArgument (khác họ địa chỉ với socket, gói quá lớn),
    // ResourceExhausted (bộ đệm gửi của hệ điều hành đầy: với UDP, bên gọi coi như gói đã mất),
    // Unavailable (không có đường tới đích), Io.
    [[nodiscard]] Result<void> send(const Address& to,
                                    std::span<const std::byte> data) const noexcept;
    // Nhận một gói vào đầu `buffer`; nullopt khi không còn gói nào. Gói lớn hơn buffer bị bỏ và trả
    // DataLoss, nên không bao giờ có gói bị cắt; lần gọi sau nhận gói kế tiếp. Lỗi khác: Io.
    [[nodiscard]] Result<std::optional<Datagram>> receive(
        std::span<std::byte> buffer) const noexcept;
    // Chờ tới khi có gói để nhận hay hết `timeout`; hạn không dương là chỉ hỏi, không chờ, và hạn
    // được làm tròn lên tới mili giây. true khi có gói, hay khi hệ điều hành có lỗi đang chờ báo
    // qua receive; false khi hết hạn. Có thể trả false trước hạn khi bị tín hiệu ngắt, nên bên gọi
    // chờ trong vòng lặp. Lỗi: FailedPrecondition (socket đã chuyển đi), Io.
    [[nodiscard]] Result<bool> wait_readable(core::Duration timeout) const noexcept;

private:
    UdpSocket(std::intptr_t handle, AddressFamily family) noexcept
        : handle_(handle), family_(family) {}

    // File descriptor hay SOCKET của hệ điều hành; -1 khi đã chuyển đi.
    std::intptr_t handle_ = -1;
    AddressFamily family_ = AddressFamily::V4;
};

}  // namespace orion::net
