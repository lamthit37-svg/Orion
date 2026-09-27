// Gói transport (docs/formats/transport.md): loại gói, header của gói dữ liệu, mã hoá AEAD với
// header làm associated data, và cửa sổ chống replay so với một mô hình ngây thơ.

#include "engine/net/packet.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/math/random.hpp"
#include "engine/net/replay_window.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace orion::net {
namespace {

constexpr u64 kU64Max = std::numeric_limits<u64>::max();

[[nodiscard]] crypto::AeadKey key_of(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kAeadKeySize> bytes{};
    bytes.fill(std::byte{fill});
    return crypto::AeadKey(bytes);
}

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text));
}

[[nodiscard]] std::vector<std::byte> seal(const u32 connection_id, const u64 sequence,
                                          const std::span<const std::byte> payload,
                                          const crypto::AeadKey& key) {
    std::vector<std::byte> packet(kMaxPacketSize);
    const Result<usize> size = seal_data_packet(packet, connection_id, sequence, payload, key);
    EXPECT_TRUE(size.has_value());
    packet.resize(size.value_or(0));
    return packet;
}

[[nodiscard]] ErrorCode open_error(const std::span<const std::byte> packet,
                                   const crypto::AeadKey& key) {
    std::array<std::byte, kMaxPacketSize> out{};
    const Result<usize> opened = open_data_packet(out, packet, key);
    EXPECT_FALSE(opened.has_value());
    return opened.has_value() ? ErrorCode::Internal : opened.error().code();
}

TEST(PacketType, ReadsTheLowNibbleOfTheFirstByte) {
    EXPECT_EQ(packet_type(std::array{std::byte{0x01}}).value_or(PacketType::Data),
              PacketType::Request);
    EXPECT_EQ(packet_type(std::array{std::byte{0x75}, std::byte{0}}).value_or(PacketType::Data),
              PacketType::Reject);
    EXPECT_EQ(packet_type(std::array{std::byte{0x16}}).value_or(PacketType::Request),
              PacketType::Data);
    for (const u8 prefix : {u8{0x00}, u8{0x07}, u8{0x0F}, u8{0xF0}}) {
        EXPECT_FALSE(packet_type(std::array{std::byte{prefix}}).has_value()) << int{prefix};
    }
    EXPECT_FALSE(packet_type({}).has_value());
    const std::vector<std::byte> too_big(kMaxPacketSize + 1, std::byte{0x06});
    EXPECT_FALSE(packet_type(too_big).has_value());
}

// Byte theo đúng bảng của đặc tả: prefix, connection id, số thứ tự ngắn nhất.
TEST(DataPacket, HeaderMatchesTheSpecification) {
    const crypto::AeadKey key = key_of(0x31);
    const std::vector<std::byte> small = seal(0x0A0B0C0D, 0x2A, bytes_of("x"), key);
    ASSERT_EQ(small.size(), 1 + 4 + 1 + 1 + crypto::kAeadTagSize);
    EXPECT_EQ(small[0], std::byte{0x06});
    EXPECT_EQ(small[1], std::byte{0x0D});
    EXPECT_EQ(small[4], std::byte{0x0A});
    EXPECT_EQ(small[5], std::byte{0x2A});
    const std::vector<std::byte> wide = seal(1, 0x0102030405060708, bytes_of("x"), key);
    EXPECT_EQ(wide[0], std::byte{0x76});
    EXPECT_EQ(wide[5], std::byte{0x08});
    EXPECT_EQ(wide[12], std::byte{0x01});
    for (const u64 sequence : {u64{0}, u64{0xFF}, u64{0x100}, u64{0xFFFF'FFFF}, kU64Max}) {
        const std::vector<std::byte> packet = seal(7, sequence, {}, key);
        const Result<DataHeader> header = read_data_header(packet);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->connection_id, 7U);
        EXPECT_EQ(header->sequence, sequence);
        EXPECT_EQ(header->size + crypto::kAeadTagSize, packet.size());
    }
}

TEST(DataPacket, RoundTripsEveryPayloadSize) {
    const crypto::AeadKey key = key_of(0x32);
    std::vector<std::byte> payload(kMaxDataPayload);
    for (usize i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i * 7U);
    }
    for (const usize size : {usize{0}, usize{1}, usize{600}, kMaxDataPayload}) {
        const std::span<const std::byte> sent = std::span(payload).first(size);
        const std::vector<std::byte> packet = seal(99, kU64Max, sent, key);
        EXPECT_LE(packet.size(), kMaxPacketSize);
        std::array<std::byte, kMaxPacketSize> out{};
        const Result<usize> opened = open_data_packet(out, packet, key);
        ASSERT_TRUE(opened.has_value());
        EXPECT_TRUE(std::ranges::equal(std::span(out).first(*opened), sent));
    }
}

TEST(DataPacket, SealRejectsWhatCannotBeSent) {
    const crypto::AeadKey key = key_of(0x33);
    std::array<std::byte, kMaxPacketSize> out{};
    EXPECT_FALSE(seal_data_packet(out, 0, 1, {}, key).has_value());
    // Số thứ tự 1 byte chở được thêm 7 byte so với kMaxDataPayload.
    const std::vector<std::byte> largest(kMaxDataPayload + 7);
    EXPECT_TRUE(seal_data_packet(out, 1, 1, largest, key).has_value());
    const std::vector<std::byte> too_large(kMaxDataPayload + 8);
    EXPECT_FALSE(seal_data_packet(out, 1, 1, too_large, key).has_value());
    EXPECT_FALSE(seal_data_packet(out, 1, kU64Max, largest, key).has_value());
    std::array<std::byte, 1 + 4 + 1 + 3 + crypto::kAeadTagSize> exact{};
    EXPECT_TRUE(seal_data_packet(exact, 1, 1, bytes_of("abc"), key).has_value());
    EXPECT_FALSE(
        seal_data_packet(std::span(exact).first(exact.size() - 1), 1, 1, bytes_of("abc"), key)
            .has_value());
}

TEST(DataPacket, HeaderRules) {
    const crypto::AeadKey key = key_of(0x34);
    const std::vector<std::byte> good = seal(5, 300, bytes_of("dữ liệu"), key);
    auto expect_invalid = [&](std::vector<std::byte> packet) {
        EXPECT_FALSE(read_data_header(packet).has_value());
        EXPECT_EQ(open_error(packet, key), ErrorCode::InvalidArgument);
    };
    std::vector<std::byte> other_type = good;
    other_type[0] = std::byte{0x15};
    expect_invalid(other_type);
    std::vector<std::byte> reserved = good;
    reserved[0] |= std::byte{0x80};
    expect_invalid(reserved);
    std::vector<std::byte> zero_id = good;
    std::fill_n(zero_id.begin() + 1, 4, std::byte{0});
    expect_invalid(zero_id);
    // Số 300 cần 2 byte; khai 3 byte (byte cao 0) là mã hoá không ngắn nhất.
    std::vector<std::byte> padded = good;
    padded[0] = std::byte{0x26};
    padded.insert(padded.begin() + 7, std::byte{0});
    expect_invalid(padded);
    expect_invalid(std::vector<std::byte>(good.begin(), good.begin() + 7 + 15));
    expect_invalid(std::vector<std::byte>(kMaxPacketSize + 1, std::byte{0x06}));
    expect_invalid({});
}

// Header là associated data: lật bất kỳ bit nào của gói cũng làm nó hỏng.
TEST(DataPacket, EveryByteIsAuthenticated) {
    const crypto::AeadKey key = key_of(0x35);
    const std::vector<std::byte> good = seal(0x01020304, 1000, bytes_of("xin chào"), key);
    for (usize i = 0; i < good.size(); ++i) {
        std::vector<std::byte> packet = good;
        packet[i] ^= std::byte{0x01};
        std::array<std::byte, kMaxPacketSize> out{};
        EXPECT_FALSE(open_data_packet(out, packet, key).has_value()) << "byte " << i;
    }
    EXPECT_EQ(open_error(good, key_of(0x36)), ErrorCode::DataLoss);
}

TEST(DataPacket, OpenNeedsRoomForThePayload) {
    const crypto::AeadKey key = key_of(0x37);
    const std::vector<std::byte> packet = seal(1, 1, bytes_of("abcd"), key);
    std::array<std::byte, 3> small{};
    const Result<usize> opened = open_data_packet(small, packet, key);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

TEST(ReplayWindow, AcceptsEachSequenceOnce) {
    ReplayWindow window;
    EXPECT_FALSE(window.highest().has_value());
    EXPECT_TRUE(window.fresh(0));
    window.record(0);
    EXPECT_FALSE(window.fresh(0));
    EXPECT_EQ(window.highest(), std::optional<u64>{0});
    window.record(5);
    EXPECT_TRUE(window.fresh(3));
    window.record(3);
    EXPECT_FALSE(window.fresh(3));
    EXPECT_FALSE(window.fresh(5));
    EXPECT_TRUE(window.fresh(4));
    EXPECT_EQ(window.highest(), std::optional<u64>{5});
}

TEST(ReplayWindow, ForgetsNothingInsideTheWindowAndRejectsOlder) {
    ReplayWindow window;
    const u64 top = 5'000;
    window.record(top);
    EXPECT_TRUE(window.fresh(top - ReplayWindow::kSize + 1));
    EXPECT_FALSE(window.fresh(top - ReplayWindow::kSize));
    EXPECT_FALSE(window.fresh(0));
    // Một bước nhảy lớn xoá hết các ô cũ.
    window.record(top + (10 * ReplayWindow::kSize));
    EXPECT_TRUE(window.fresh(top + (10 * ReplayWindow::kSize) - 1));
    EXPECT_FALSE(window.fresh(top));
}

TEST(ReplayWindow, WorksAtTheTopOfTheSequenceSpace) {
    ReplayWindow window;
    window.record(kU64Max - 1);
    window.record(kU64Max);
    EXPECT_FALSE(window.fresh(kU64Max));
    EXPECT_FALSE(window.fresh(kU64Max - 1));
    EXPECT_TRUE(window.fresh(kU64Max - 2));
    EXPECT_FALSE(window.fresh(0));
}

// So với mô hình ngây thơ trên 200 000 thao tác sinh bằng PCG32 seed cố định (X.4): số trong khoảng
// ±1 500 quanh đỉnh, cộng thỉnh thoảng một bước nhảy xa.
TEST(ReplayWindow, MatchesANaiveModel) {
    ReplayWindow window;
    std::set<u64> recorded;
    std::optional<u64> highest;
    math::Pcg32 rng(20'260'927, 7);
    u64 base = 1'000'000;
    for (u32 step = 0; step < 200'000; ++step) {
        if (rng.next_below(1'000) == 0) {
            base += rng.next_below(5'000);
        }
        const u64 sequence = base + rng.next_below(3'000) - 1'500;
        const bool expected =
            !highest.has_value() || sequence > *highest ||
            (*highest - sequence < ReplayWindow::kSize && !recorded.contains(sequence));
        ASSERT_EQ(window.fresh(sequence), expected) << "bước " << step << ", số " << sequence;
        if (rng.next_below(4) != 0) {
            window.record(sequence);
            if (expected) {
                recorded.insert(sequence);
                highest = highest.has_value() ? std::max(*highest, sequence) : sequence;
            }
        }
        ASSERT_EQ(window.highest(), highest);
    }
}

}  // namespace
}  // namespace orion::net
