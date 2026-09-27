// Khối dùng chung của kết nối: SecureChannel, payload có kind, và Outbox
// (docs/formats/transport.md, mục Gói dữ liệu và Kết nối).

#include "engine/net/connection.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/net/address.hpp"
#include "engine/net/outbox.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/secure_channel.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::net {
namespace {

constexpr u64 kU64Max = std::numeric_limits<u64>::max();

// Hai kênh của một kết nối: khoá gửi của bên này là khoá nhận của bên kia.
struct ChannelPair {
    SecureChannel client;
    SecureChannel server;
};

[[nodiscard]] ChannelPair channel_pair(const u32 connection_id, const u64 first_sequence = 0) {
    EXPECT_TRUE(crypto::initialize().has_value());
    const crypto::KeyExchangeKeyPair client_key = crypto::generate_key_exchange_key_pair();
    const crypto::KeyExchangeKeyPair server_key = crypto::generate_key_exchange_key_pair();
    Result<crypto::SessionKeys> client_keys =
        crypto::client_session_keys(client_key, server_key.public_key);
    Result<crypto::SessionKeys> server_keys =
        crypto::server_session_keys(server_key, client_key.public_key);
    EXPECT_TRUE(client_keys.has_value() && server_keys.has_value());
    return {.client = SecureChannel(connection_id, std::move(*client_keys), first_sequence),
            .server = SecureChannel(connection_id, std::move(*server_keys))};
}

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text));
}

[[nodiscard]] std::vector<std::byte> seal(SecureChannel& channel,
                                          const std::span<const std::byte> payload) {
    std::vector<std::byte> packet(kMaxPacketSize);
    const Result<usize> size = channel.seal(packet, payload);
    EXPECT_TRUE(size.has_value());
    packet.resize(size.value_or(0));
    return packet;
}

TEST(SecureChannel, RoundTripAndSequencesCountUp) {
    ChannelPair pair = channel_pair(77);
    std::array<std::byte, kMaxPacketSize> out{};
    for (u64 i = 0; i < 3; ++i) {
        const std::vector<std::byte> packet = seal(pair.client, bytes_of("dữ liệu"));
        const Result<DataHeader> header = read_data_header(packet);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->sequence, i);
        const Result<usize> opened = pair.server.open(out, packet);
        ASSERT_TRUE(opened.has_value());
        EXPECT_TRUE(std::ranges::equal(std::span(out).first(*opened), bytes_of("dữ liệu")));
        EXPECT_EQ(pair.server.highest_received(), std::optional<u64>{i});
    }
}

TEST(SecureChannel, OpenRejectsReplaysOtherConnectionsAndForgeries) {
    ChannelPair pair = channel_pair(5);
    ChannelPair other = channel_pair(6);
    std::array<std::byte, kMaxPacketSize> out{};
    const std::vector<std::byte> first = seal(pair.client, bytes_of("a"));
    const std::vector<std::byte> second = seal(pair.client, bytes_of("b"));

    std::vector<std::byte> forged = second;
    forged.back() ^= std::byte{1};
    EXPECT_EQ(pair.server.open(out, forged).error().code(), ErrorCode::DataLoss);
    EXPECT_FALSE(pair.server.highest_received().has_value());  // Cửa sổ không đổi khi lỗi.

    EXPECT_EQ(other.server.open(out, first).error().code(), ErrorCode::InvalidArgument);
    ASSERT_TRUE(pair.server.open(out, second).has_value());
    ASSERT_TRUE(pair.server.open(out, first).has_value());  // Tới trễ nhưng trong cửa sổ.
    EXPECT_EQ(pair.server.open(out, first).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(pair.server.open(out, second).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(pair.server.open(out, std::span(first).first(5)).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(SecureChannel, LastSequenceIsUsedOnceThenTheChannelIsExhausted) {
    ChannelPair pair = channel_pair(9, kU64Max);
    std::array<std::byte, kMaxPacketSize> packet{};
    const Result<usize> size = pair.client.seal(packet, bytes_of("cuối"));
    ASSERT_TRUE(size.has_value());
    std::array<std::byte, kMaxPacketSize> out{};
    ASSERT_TRUE(pair.server.open(out, std::span(packet).first(*size)).has_value());
    EXPECT_EQ(pair.server.highest_received(), std::optional<u64>{kU64Max});
    const Result<usize> again = pair.client.seal(packet, bytes_of("nữa"));
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::ResourceExhausted);
}

TEST(SecureChannel, FailedSealDoesNotConsumeASequence) {
    ChannelPair pair = channel_pair(3);
    std::array<std::byte, 8> tiny{};
    EXPECT_EQ(pair.client.seal(tiny, bytes_of("x")).error().code(), ErrorCode::InvalidArgument);
    const std::vector<std::byte> packet = seal(pair.client, bytes_of("x"));
    const Result<DataHeader> header = read_data_header(packet);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->sequence, 0U);
}

TEST(Payload, ParseAcceptsExactlyTheThreeKinds) {
    const std::array keep_alive = {std::byte{0}};
    const std::array data = {std::byte{1}, std::byte{'a'}, std::byte{'b'}};
    const std::array disconnect = {std::byte{2}};
    // Giá trị mặc định khác kind cần kiểm, để nullopt làm test hỏng.
    const Payload missing{.kind = PayloadKind::Disconnect, .data = {}};
    EXPECT_EQ(parse_payload(keep_alive).value_or(missing).kind, PayloadKind::KeepAlive);
    const Payload parsed = parse_payload(data).value_or(missing);
    EXPECT_EQ(parsed.kind, PayloadKind::Data);
    EXPECT_EQ(parsed.data.size(), 2U);
    EXPECT_EQ(parse_payload(disconnect).value_or(Payload{}).kind, PayloadKind::Disconnect);

    const std::array long_keep_alive = {std::byte{0}, std::byte{0}};
    const std::array empty_data = {std::byte{1}};
    const std::array long_disconnect = {std::byte{2}, std::byte{9}};
    const std::array unknown = {std::byte{3}};
    EXPECT_FALSE(parse_payload({}).has_value());
    EXPECT_FALSE(parse_payload(long_keep_alive).has_value());
    EXPECT_FALSE(parse_payload(empty_data).has_value());
    EXPECT_FALSE(parse_payload(long_disconnect).has_value());
    EXPECT_FALSE(parse_payload(unknown).has_value());
}

TEST(Payload, SealChecksDataSizeAgainstKind) {
    ChannelPair pair = channel_pair(4);
    std::array<std::byte, kMaxPacketSize> packet{};
    EXPECT_TRUE(seal_payload(pair.client, PayloadKind::KeepAlive, {}, packet).has_value());
    EXPECT_FALSE(seal_payload(pair.client, PayloadKind::KeepAlive, bytes_of("x"), packet));
    EXPECT_FALSE(seal_payload(pair.client, PayloadKind::Data, {}, packet).has_value());
    const std::vector<std::byte> largest(kMaxTransportPayload, std::byte{1});
    const Result<usize> sealed = seal_payload(pair.client, PayloadKind::Data, largest, packet);
    ASSERT_TRUE(sealed.has_value());
    EXPECT_LE(*sealed, kMaxPacketSize);
    const std::vector<std::byte> too_large(kMaxTransportPayload + 1);
    EXPECT_FALSE(seal_payload(pair.client, PayloadKind::Data, too_large, packet).has_value());
    // Bên kia mở ra đúng kind và dữ liệu. Gói KeepAlive đầu tiên bị bỏ qua: chỉ gói cuối được mở.
    std::array<std::byte, kMaxPacketSize> out{};
    const Result<usize> opened = pair.server.open(out, std::span(packet).first(*sealed));
    ASSERT_TRUE(opened.has_value());
    const Payload payload = parse_payload(std::span(out).first(*opened)).value_or(Payload{});
    EXPECT_EQ(payload.kind, PayloadKind::Data);
    EXPECT_TRUE(std::ranges::equal(payload.data, largest));
}

TEST(Outbox, KeepsPacketsInOrderAndCountsDrops) {
    Outbox outbox(2);
    const Address a = Address::loopback_v4(1);
    const Address b = Address::loopback_v4(2);
    EXPECT_TRUE(outbox.empty());
    EXPECT_TRUE(outbox.push(a, bytes_of("một")));
    EXPECT_TRUE(outbox.push(b, bytes_of("hai")));
    EXPECT_FALSE(outbox.push(a, bytes_of("ba")));
    EXPECT_EQ(outbox.dropped(), 1U);
    ASSERT_EQ(outbox.packets().size(), 2U);
    EXPECT_EQ(outbox.packets()[0].to, a);
    EXPECT_TRUE(std::ranges::equal(outbox.packets()[1].view(), bytes_of("hai")));
    outbox.clear();
    EXPECT_TRUE(outbox.empty());
    const std::vector<std::byte> largest(kMaxPacketSize, std::byte{0x7E});
    EXPECT_TRUE(outbox.push(a, largest));
    EXPECT_EQ(outbox.packets()[0].size, kMaxPacketSize);
    EXPECT_EQ(outbox.dropped(), 1U);
}

}  // namespace
}  // namespace orion::net
