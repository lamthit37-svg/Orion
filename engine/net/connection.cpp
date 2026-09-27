#include "engine/net/connection.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/secure_channel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace orion::net {

std::optional<Payload> parse_payload(const std::span<const std::byte> plaintext) noexcept {
    if (plaintext.empty()) {
        return std::nullopt;
    }
    const auto kind = std::to_integer<u8>(plaintext[0]);
    if (kind == std::to_underlying(PayloadKind::Data)) {
        if (plaintext.size() < 2) {
            return std::nullopt;
        }
        return Payload{.kind = PayloadKind::Data, .data = plaintext.subspan(1)};
    }
    if (plaintext.size() != 1 || (kind != std::to_underlying(PayloadKind::KeepAlive) &&
                                  kind != std::to_underlying(PayloadKind::Disconnect))) {
        return std::nullopt;
    }
    return Payload{.kind = static_cast<PayloadKind>(kind), .data = {}};
}

Result<usize> seal_payload(SecureChannel& channel, const PayloadKind kind,
                           const std::span<const std::byte> data,
                           const std::span<std::byte, kMaxPacketSize> packet) noexcept {
    const bool is_data = kind == PayloadKind::Data;
    if (is_data ? (data.empty() || data.size() > kMaxTransportPayload) : !data.empty()) {
        return fail(ErrorCode::InvalidArgument, "net: dữ liệu sai cỡ với kind của payload",
                    static_cast<i64>(data.size()));
    }
    std::array<std::byte, kMaxDataPayload> plaintext{};
    plaintext[0] = static_cast<std::byte>(std::to_underlying(kind));
    std::ranges::copy(data, plaintext.begin() + 1);
    return channel.seal(packet, std::span(plaintext).first(1 + data.size()));
}

}  // namespace orion::net
