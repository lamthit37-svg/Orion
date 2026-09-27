#pragma once

// Hàng gói chờ gửi của transport: client và server ghi gói trả lời, gói dữ liệu và keep-alive vào
// đây, luồng IO lấy ra gửi qua UdpSocket rồi clear. Nhờ vậy client và server không chạm socket
// (test chạy tất định trên mạng giả, X.4). Bộ nhớ cấp lúc dựng theo sức chứa cố định; push không
// cấp phát (X.7). Hàng đầy thì gói mới bị bỏ và được đếm, như một gói UDP mất. Không đồng bộ.

#include "engine/core/types.hpp"
#include "engine/net/address.hpp"
#include "engine/net/packet.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace orion::net {

struct OutgoingPacket {
    Address to;
    usize size = 0;
    std::array<std::byte, kMaxPacketSize> bytes{};

    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        return std::span(bytes).first(size);
    }
};

class Outbox {
public:
    explicit Outbox(usize capacity);

    // Chép `packet` (tối đa kMaxPacketSize byte) vào hàng. false khi hàng đầy: gói bị bỏ và đếm.
    bool push(const Address& to, std::span<const std::byte> packet) noexcept;

    [[nodiscard]] std::span<const OutgoingPacket> packets() const noexcept {
        return std::span(packets_).first(size_);
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    // Bỏ mọi gói đã lấy ra gửi.
    void clear() noexcept { size_ = 0; }
    // Số gói bị bỏ vì hàng đầy, từ lúc dựng.
    [[nodiscard]] u64 dropped() const noexcept { return dropped_; }

private:
    std::vector<OutgoingPacket> packets_;
    usize size_ = 0;
    u64 dropped_ = 0;
};

}  // namespace orion::net
