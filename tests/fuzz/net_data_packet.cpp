// Fuzz gói dữ liệu và cửa sổ chống replay (CLAUDE.md X.4, X.9; docs/formats/transport.md): mọi gói
// UDP gateway nhận đi qua read_data_header và open_data_packet trước khi xác thực. Fuzz không giả
// được tag, nên hai bit thấp của byte đầu chọn cách dùng phần còn lại:
//   0: một gói từ mạng. open với khoá của harness phải trả lỗi trong bảng "Lỗi"; header đọc được
//      thì đúng mọi luật (connection id khác 0, số thứ tự mã hoá ngắn nhất, đủ chỗ cho tag).
//   1: 4 byte connection id, 8 byte số thứ tự, rồi payload: harness niêm phong rồi mở lại, và phải
//      ra đúng payload, connection id, số thứ tự.
//   2: dãy thao tác trên cửa sổ chống replay, mỗi thao tác 9 byte (byte cờ, số thứ tự u64), so với
//      một mô hình ngây thơ.
//   3: như 0.

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/replay_window.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace {

using orion::u32;
using orion::u64;
using orion::usize;

[[nodiscard]] orion::crypto::AeadKey harness_key() {
    ORION_VERIFY(orion::crypto::initialize().has_value(), "sodium_init thất bại");
    std::array<std::byte, orion::crypto::kAeadKeySize> bytes{};
    bytes.fill(std::byte{0x5A});
    return orion::crypto::AeadKey(bytes);
}

void fuzz_network_packet(const std::span<const std::byte> packet,
                         const orion::crypto::AeadKey& key) {
    namespace net = orion::net;
    using orion::ErrorCode;
    const orion::Result<net::DataHeader> header = net::read_data_header(packet);
    if (header.has_value()) {
        const usize length = header->size - 5;
        const usize shortest = header->sequence == 0
                                   ? 1
                                   : (static_cast<usize>(std::bit_width(header->sequence)) + 7) / 8;
        ORION_VERIFY(header->connection_id != 0 && length >= 1 && length <= 8 &&
                         length == shortest &&
                         packet.size() >= header->size + orion::crypto::kAeadTagSize &&
                         packet.size() <= net::kMaxPacketSize,
                     "header được nhận mà sai luật");
    } else {
        ORION_VERIFY(header.error().code() == ErrorCode::InvalidArgument,
                     "read_data_header trả mã lỗi ngoài bảng");
    }
    std::array<std::byte, net::kMaxPacketSize> out{};
    const orion::Result<usize> opened = net::open_data_packet(out, packet, key);
    ORION_VERIFY(!opened.has_value(), "fuzz mở được gói không do harness niêm phong");
    ORION_VERIFY(opened.error().code() ==
                     (header.has_value() ? ErrorCode::DataLoss : ErrorCode::InvalidArgument),
                 "open_data_packet trả mã lỗi ngoài bảng");
}

void fuzz_round_trip(const std::span<const std::byte> input, const orion::crypto::AeadKey& key) {
    namespace net = orion::net;
    if (input.size() < 12) {
        return;
    }
    const auto connection_id = orion::core::load_le<u32>(input.first<4>());
    const auto sequence = orion::core::load_le<u64>(input.subspan<4, 8>());
    const std::span<const std::byte> payload = input.subspan(12);
    std::array<std::byte, net::kMaxPacketSize> packet{};
    const orion::Result<usize> sealed =
        net::seal_data_packet(packet, connection_id, sequence, payload, key);
    if (!sealed.has_value()) {
        ORION_VERIFY(connection_id == 0 || payload.size() > net::kMaxDataPayload,
                     "seal từ chối gói gửi được");
        return;
    }
    const std::span<const std::byte> wire = std::span(packet).first(*sealed);
    const orion::Result<net::DataHeader> header = net::read_data_header(wire);
    ORION_VERIFY(header.has_value() && header->connection_id == connection_id &&
                     header->sequence == sequence,
                 "header đọc lại khác header đã ghi");
    std::array<std::byte, net::kMaxPacketSize> out{};
    const orion::Result<usize> opened = net::open_data_packet(out, wire, key);
    ORION_VERIFY(opened.has_value() && std::ranges::equal(std::span(out).first(*opened), payload),
                 "mở lại không ra đúng payload");
}

void fuzz_replay_window(std::span<const std::byte> input) {
    orion::net::ReplayWindow window;
    std::set<u64> recorded;
    std::optional<u64> highest;
    while (input.size() >= 9) {
        const bool record = (std::to_integer<u32>(input[0]) & 1U) != 0;
        const auto sequence = orion::core::load_le<u64>(input.subspan<1, 8>());
        input = input.subspan(9);
        const bool expected =
            !highest.has_value() || sequence > *highest ||
            (*highest - sequence < orion::net::ReplayWindow::kSize && !recorded.contains(sequence));
        ORION_VERIFY(window.fresh(sequence) == expected, "cửa sổ khác mô hình");
        if (record) {
            window.record(sequence);
            if (expected) {
                recorded.insert(sequence);
                highest = highest.has_value() ? std::max(*highest, sequence) : sequence;
            }
        }
        ORION_VERIFY(window.highest() == highest, "số lớn nhất khác mô hình");
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const orion::crypto::AeadKey kKey = harness_key();
    if (size == 0) {
        return 0;
    }
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size - 1);
    if (!input.empty()) {
        std::memcpy(input.data(), data + 1, input.size());
    }
    switch (data[0] & 3U) {
        case 1:
            fuzz_round_trip(input, kKey);
            break;
        case 2:
            fuzz_replay_window(input);
            break;
        default:
            fuzz_network_packet(input, kKey);
            break;
    }
    return 0;
}
