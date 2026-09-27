#include "engine/net/packet.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"

#include <bit>
#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace orion::net {
namespace {

// Bố cục của prefix và của header gói dữ liệu (docs/formats/transport.md).
constexpr u8 kTypeMask = 0x0F;
constexpr u32 kSequenceLengthShift = 4;
constexpr u8 kSequenceLengthMask = 0x07;
constexpr u8 kReservedBit = 0x80;
constexpr usize kConnectionIdOffset = 1;
constexpr usize kSequenceOffset = 5;
constexpr usize kMinDataPacketSize = kSequenceOffset + 1 + crypto::kAeadTagSize;
// Kênh của nonce cho gói dữ liệu: 4 byte 0 trước số thứ tự.
constexpr u32 kDataNonceChannel = 0;

// Số byte ngắn nhất chứa `sequence`, từ 1 tới 8.
[[nodiscard]] constexpr usize sequence_length(const u64 sequence) noexcept {
    const auto bits = static_cast<usize>(std::bit_width(sequence));
    return bits == 0 ? 1 : (bits + 7) / 8;
}

static_assert(sequence_length(0) == 1 && sequence_length(0xFF) == 1 &&
              sequence_length(0x100) == 2 && sequence_length(~u64{0}) == 8);

}  // namespace

Result<PacketType> packet_type(const std::span<const std::byte> packet) noexcept {
    if (packet.empty() || packet.size() > kMaxPacketSize) {
        return fail(ErrorCode::InvalidArgument, "net: gói rỗng hay quá lớn",
                    static_cast<i64>(packet.size()));
    }
    const auto type = static_cast<u8>(std::to_integer<u8>(packet[0]) & kTypeMask);
    if (type < std::to_underlying(PacketType::Request) ||
        type > std::to_underlying(PacketType::Data)) {
        return fail(ErrorCode::InvalidArgument, "net: loại gói lạ", type);
    }
    return static_cast<PacketType>(type);
}

Result<DataHeader> read_data_header(const std::span<const std::byte> packet) noexcept {
    if (packet.size() < kMinDataPacketSize || packet.size() > kMaxPacketSize) {
        return fail(ErrorCode::InvalidArgument, "net: cỡ gói dữ liệu sai",
                    static_cast<i64>(packet.size()));
    }
    const auto prefix = std::to_integer<u8>(packet[0]);
    if ((prefix & kTypeMask) != std::to_underlying(PacketType::Data) ||
        (prefix & kReservedBit) != 0) {
        return fail(ErrorCode::InvalidArgument, "net: prefix của gói dữ liệu sai", prefix);
    }
    const usize length =
        static_cast<usize>((prefix >> kSequenceLengthShift) & kSequenceLengthMask) + 1;
    DataHeader header;
    header.size = kSequenceOffset + length;
    if (packet.size() < header.size + crypto::kAeadTagSize) {
        return fail(ErrorCode::InvalidArgument, "net: gói dữ liệu ngắn hơn header và tag",
                    static_cast<i64>(packet.size()));
    }
    header.connection_id = core::load_le<u32>(packet.subspan<kConnectionIdOffset, sizeof(u32)>());
    if (header.connection_id == 0) {
        return fail(ErrorCode::InvalidArgument, "net: connection id là 0");
    }
    for (usize i = 0; i < length; ++i) {
        header.sequence |= std::to_integer<u64>(packet[kSequenceOffset + i]) << (8U * i);
    }
    if (sequence_length(header.sequence) != length) {
        return fail(ErrorCode::InvalidArgument, "net: số thứ tự không mã hoá ngắn nhất",
                    static_cast<i64>(length));
    }
    return header;
}

Result<usize> seal_data_packet(const std::span<std::byte> out, const u32 connection_id,
                               const u64 sequence, const std::span<const std::byte> payload,
                               const crypto::AeadKey& key) noexcept {
    if (connection_id == 0) {
        return fail(ErrorCode::InvalidArgument, "net: connection id là 0");
    }
    const usize length = sequence_length(sequence);
    const usize header_size = kSequenceOffset + length;
    if (payload.size() > kMaxPacketSize - header_size - crypto::kAeadTagSize) {
        return fail(ErrorCode::InvalidArgument, "net: payload quá lớn cho một gói",
                    static_cast<i64>(payload.size()));
    }
    const usize total = header_size + payload.size() + crypto::kAeadTagSize;
    if (out.size() < total) {
        return fail(ErrorCode::InvalidArgument, "net: bộ đệm ra thiếu chỗ",
                    static_cast<i64>(out.size()));
    }
    out[0] = static_cast<std::byte>(std::to_underlying(PacketType::Data) |
                                    ((length - 1) << kSequenceLengthShift));
    core::store_le<u32>(out.subspan<kConnectionIdOffset, sizeof(u32)>(), connection_id);
    for (usize i = 0; i < length; ++i) {
        out[kSequenceOffset + i] = static_cast<std::byte>((sequence >> (8U * i)) & 0xFFU);
    }
    const Result<usize> sealed = crypto::seal(
        out.subspan(header_size, payload.size() + crypto::kAeadTagSize), payload,
        out.first(header_size), crypto::nonce_from_sequence(kDataNonceChannel, sequence), key);
    // Cỡ của payload và của bộ đệm ra đã kiểm ở trên, nên seal không có lý do gì để lỗi.
    ORION_VERIFY(sealed.has_value(), "net: seal lỗi dù cỡ đã kiểm");
    return header_size + *sealed;
}

Result<usize> open_data_packet(const std::span<std::byte> out,
                               const std::span<const std::byte> packet,
                               const crypto::AeadKey& key) noexcept {
    const Result<DataHeader> header = read_data_header(packet);
    if (!header) {
        return std::unexpected(header.error());
    }
    const std::span<const std::byte> ciphertext = packet.subspan(header->size);
    if (out.size() < ciphertext.size() - crypto::kAeadTagSize) {
        return fail(ErrorCode::InvalidArgument, "net: bộ đệm ra thiếu chỗ",
                    static_cast<i64>(out.size()));
    }
    return crypto::open(out, ciphertext, packet.first(header->size),
                        crypto::nonce_from_sequence(kDataNonceChannel, header->sequence), key);
}

}  // namespace orion::net
