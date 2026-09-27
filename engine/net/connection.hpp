#pragma once

// Phần chung của một kết nối transport cho client và server (docs/formats/transport.md, mục Kết
// nối): byte kind đầu payload, cỡ dữ liệu tầng trên tối đa, các hạn thời gian, và cách đóng gói
// payload qua SecureChannel.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/secure_channel.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace orion::net {

// Byte đầu của payload trong gói dữ liệu.
enum class PayloadKind : u8 {
    KeepAlive = 0,
    Data = 1,
    Disconnect = 2,
};

// Dữ liệu tầng trên lớn nhất trong một gói: payload trừ byte kind.
inline constexpr usize kMaxTransportPayload = kMaxDataPayload - 1;

// Chính sách, không phải số đo (mục Kết nối của định dạng).
inline constexpr core::Duration kHandshakeResendInterval = core::Duration::milliseconds(250);
inline constexpr core::Duration kHandshakeTimeout = core::Duration::seconds(10);
inline constexpr core::Duration kPendingTimeout = core::Duration::seconds(10);
inline constexpr core::Duration kIdleTimeout = core::Duration::seconds(10);
inline constexpr core::Duration kKeepAliveInterval = core::Duration::seconds(1);
inline constexpr u32 kDisconnectRepeats = 3;

struct Payload {
    PayloadKind kind = PayloadKind::KeepAlive;
    // Chỉ khác rỗng với Data.
    std::span<const std::byte> data;
};

// Đọc payload đã giải mã. nullopt khi sai luật: kind lạ, KeepAlive hay Disconnect dài hơn 1 byte,
// Data không có dữ liệu.
[[nodiscard]] std::optional<Payload> parse_payload(std::span<const std::byte> plaintext) noexcept;

// Niêm phong một payload thành gói dữ liệu qua `channel`, ghi vào đầu `packet`, trả cỡ gói. `data`
// chỉ dùng với Data (1 tới kMaxTransportPayload byte). Lỗi như SecureChannel::seal, và
// InvalidArgument khi `data` sai cỡ với kind.
[[nodiscard]] Result<usize> seal_payload(SecureChannel& channel, PayloadKind kind,
                                         std::span<const std::byte> data,
                                         std::span<std::byte, kMaxPacketSize> packet) noexcept;

}  // namespace orion::net
