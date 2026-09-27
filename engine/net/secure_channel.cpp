#include "engine/net/secure_channel.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/net/packet.hpp"

#include <cstddef>
#include <expected>
#include <limits>
#include <span>
#include <utility>

namespace orion::net {

SecureChannel::SecureChannel(const u32 connection_id, crypto::SessionKeys keys,
                             const u64 first_sequence) noexcept
    : connection_id_(connection_id), keys_(std::move(keys)), next_sequence_(first_sequence) {}

Result<usize> SecureChannel::seal(const std::span<std::byte> out,
                                  const std::span<const std::byte> payload) noexcept {
    if (exhausted_) {
        return fail(ErrorCode::ResourceExhausted, "net: hết số thứ tự, kết nối phải đóng");
    }
    const Result<usize> sealed =
        seal_data_packet(out, connection_id_, next_sequence_, payload, keys_.transmit);
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    if (next_sequence_ == std::numeric_limits<u64>::max()) {
        exhausted_ = true;
    } else {
        ++next_sequence_;
    }
    return *sealed;
}

Result<usize> SecureChannel::open(const std::span<std::byte> out,
                                  const std::span<const std::byte> packet) noexcept {
    const Result<DataHeader> header = read_data_header(packet);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->connection_id != connection_id_) {
        return fail(ErrorCode::InvalidArgument, "net: gói của kết nối khác");
    }
    if (!window_.fresh(header->sequence)) {
        return fail(ErrorCode::AlreadyExists, "net: gói lặp lại hay cũ hơn cửa sổ");
    }
    const Result<usize> opened = open_data_packet(out, packet, keys_.receive);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    window_.record(header->sequence);
    return *opened;
}

}  // namespace orion::net
