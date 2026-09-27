// Lớp kênh tin nhắn (docs/formats/channels.md). Mạng giả trong file này mất và đảo gói theo PCG32
// seed cố định, thời gian là đồng hồ giả (X.4).

#include "engine/net/channels.hpp"

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/math/random.hpp"
#include "engine/net/bitstream.hpp"
#include "engine/net/connection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::net {
namespace {

using core::Duration;
using core::MonoTime;

using Bytes = std::vector<std::byte>;

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text));
}

[[nodiscard]] Bytes pattern(const usize size, const u32 seed) {
    Bytes bytes(size);
    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 31U) + (usize{seed} * 7U));
    }
    return bytes;
}

constexpr usize kTestBuffer = usize{64} * 1024;

// Hai đầu dùng cùng một cỡ bộ đệm cho cả hai chiều.
[[nodiscard]] MessageChannels make_channels(const usize buffer_size = kTestBuffer) {
    Result<MessageChannels> channels = MessageChannels::create(
        {.receive_buffer_size = buffer_size, .send_buffer_size = buffer_size});
    EXPECT_TRUE(channels.has_value());
    return std::move(*channels);
}

// Ghi một gói (có thể rỗng khi không có gì cần gửi).
[[nodiscard]] Bytes write(MessageChannels& channels, const MonoTime now) {
    std::array<std::byte, kMaxTransportPayload> buffer{};
    const usize size = channels.write_packet(buffer, now);
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)};
}

struct Delivery {
    Channel channel;
    Bytes data;
};

[[nodiscard]] std::vector<Delivery> read(MessageChannels& channels, const Bytes& packet,
                                         const MonoTime now) {
    const Result<std::span<const ReceivedMessage>> messages = channels.read_packet(packet, now);
    EXPECT_TRUE(messages.has_value());
    std::vector<Delivery> out;
    for (const ReceivedMessage& message : messages.value_or(std::span<const ReceivedMessage>{})) {
        out.push_back({message.channel, Bytes(message.data.begin(), message.data.end())});
    }
    return out;
}

constexpr MonoTime kStart = MonoTime::from_nanoseconds(1'000'000'000);

TEST(MessageChannels, OneMessageOnEachChannelArrives) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    for (const Channel channel : {Channel::Unreliable, Channel::Sequenced, Channel::ReliableOrdered,
                                  Channel::ReliableUnordered}) {
        ASSERT_TRUE(a.send(channel, bytes_of("tin nhắn")).has_value());
    }
    const Bytes packet = write(a, kStart);
    ASSERT_FALSE(packet.empty());
    EXPECT_LE(packet.size(), kMaxTransportPayload);
    EXPECT_TRUE(write(a, kStart).empty());  // Không còn gì cần gửi ngay.
    const std::vector<Delivery> got = read(b, packet, kStart);
    ASSERT_EQ(got.size(), 4U);
    std::set<Channel> channels;
    for (const Delivery& delivery : got) {
        channels.insert(delivery.channel);
        EXPECT_TRUE(std::ranges::equal(delivery.data, bytes_of("tin nhắn")));
    }
    EXPECT_EQ(channels.size(), 4U);
}

TEST(MessageChannels, SendChecksSizesAndConfig) {
    EXPECT_FALSE(
        MessageChannels::create({.receive_buffer_size = 0, .send_buffer_size = 10}).has_value());
    EXPECT_FALSE(MessageChannels::create(
                     {.receive_buffer_size = 10, .send_buffer_size = kMaxReliableBufferSize + 1})
                     .has_value());
    // Bộ đệm lớn nhất vẫn không cho tin nhắn quá 32 mảnh.
    MessageChannels largest = make_channels(kMaxReliableBufferSize);
    EXPECT_TRUE(largest.send(Channel::ReliableUnordered, Bytes(kMaxReliableMessageSize)));
    EXPECT_EQ(
        largest.send(Channel::ReliableUnordered, Bytes(kMaxReliableMessageSize + 1)).error().code(),
        ErrorCode::InvalidArgument);
    // Bộ đệm nhỏ hơn 32 KiB giới hạn cỡ tin nhắn tin cậy.
    MessageChannels channels = make_channels(2'000);
    EXPECT_EQ(channels.send(Channel::Unreliable, {}).error().code(), ErrorCode::InvalidArgument);
    const Bytes largest_unreliable(kMaxUnreliableMessageSize);
    EXPECT_TRUE(channels.send(Channel::Sequenced, largest_unreliable).has_value());
    const Bytes too_large(kMaxUnreliableMessageSize + 1);
    EXPECT_EQ(channels.send(Channel::Unreliable, too_large).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_TRUE(channels.send(Channel::ReliableOrdered, Bytes(2'000)).has_value());
    EXPECT_EQ(channels.send(Channel::ReliableOrdered, Bytes(2'001)).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(MessageChannels, UnreliableQueueKeepsOrderAcrossPackets) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(a.send(Channel::Unreliable, pattern(1'000, i)).has_value());
    }
    std::vector<Bytes> received;
    for (u32 round = 0; round < 3; ++round) {
        const Bytes packet = write(a, kStart);
        ASSERT_FALSE(packet.empty());
        for (Delivery& delivery : read(b, packet, kStart)) {
            received.push_back(std::move(delivery.data));
        }
    }
    ASSERT_EQ(received.size(), 3U);
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(received[i], pattern(1'000, i));
    }
    // Hàng đầy theo số tin nhắn.
    for (u32 i = 0; i < kMaxMessagesPerPacket; ++i) {
        ASSERT_TRUE(a.send(Channel::Unreliable, bytes_of("x")).has_value());
    }
    EXPECT_EQ(a.send(Channel::Unreliable, bytes_of("x")).error().code(),
              ErrorCode::ResourceExhausted);
}

TEST(MessageChannels, SequencedDropsOlderMessages) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    std::vector<Bytes> packets;
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(a.send(Channel::Sequenced, pattern(10, i)).has_value());
        packets.push_back(write(a, kStart));
    }
    std::vector<Delivery> got = read(b, packets[2], kStart);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].data, pattern(10, 2));
    EXPECT_TRUE(read(b, packets[1], kStart).empty());
    EXPECT_TRUE(read(b, packets[0], kStart).empty());
}

TEST(MessageChannels, OrderedWaitsForTheGapAndUnorderedDoesNot) {
    for (const Channel channel : {Channel::ReliableOrdered, Channel::ReliableUnordered}) {
        MessageChannels a = make_channels();
        MessageChannels b = make_channels();
        std::vector<Bytes> packets;
        for (u32 i = 0; i < 3; ++i) {
            ASSERT_TRUE(a.send(channel, pattern(20, i)).has_value());
            packets.push_back(write(a, kStart));
        }
        std::vector<Bytes> order;
        for (const usize index : {usize{2}, usize{0}, usize{1}}) {
            for (Delivery& delivery : read(b, packets[index], kStart)) {
                order.push_back(std::move(delivery.data));
            }
            if (channel == Channel::ReliableOrdered && index == 2) {
                EXPECT_TRUE(order.empty());
            }
        }
        ASSERT_EQ(order.size(), 3U);
        const std::vector<u32> expected = channel == Channel::ReliableOrdered
                                              ? std::vector<u32>{0, 1, 2}
                                              : std::vector<u32>{2, 0, 1};
        for (usize i = 0; i < 3; ++i) {
            EXPECT_EQ(order[i], pattern(20, expected[i]));
        }
        // Gói lặp lại (qua một gói transport khác) không giao lại.
        EXPECT_TRUE(read(b, packets[0], kStart).empty());
    }
}

TEST(MessageChannels, LargeMessagesAreFragmentedAndReassembled) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    const Bytes message = pattern(kMaxReliableMessageSize, 5);
    ASSERT_TRUE(a.send(Channel::ReliableOrdered, message).has_value());
    std::vector<Bytes> packets;
    for (Bytes packet = write(a, kStart); !packet.empty(); packet = write(a, kStart)) {
        packets.push_back(std::move(packet));
    }
    ASSERT_EQ(packets.size(), kMaxFragments);  // Một mảnh 1024 byte mỗi gói.
    std::ranges::reverse(packets);
    std::vector<Delivery> got;
    for (const Bytes& packet : packets) {
        std::vector<Delivery> part = read(b, packet, kStart);
        got.insert(got.end(), part.begin(), part.end());
    }
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].data, message);
}

// Gói đầu bị mất: mảnh được gửi lại sau max(50 ms, 1,5 × RTT); khi được xác nhận thì thôi.
TEST(MessageChannels, UnackedReliableIsResentUntilAcked) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    ASSERT_TRUE(a.send(Channel::ReliableUnordered, bytes_of("quan trọng")).has_value());
    ASSERT_FALSE(write(a, kStart).empty());  // Mất.
    EXPECT_TRUE(write(a, kStart + Duration::milliseconds(149)).empty());
    const MonoTime resend = kStart + Duration::milliseconds(150);  // 1,5 × RTT đầu 100 ms
    const Bytes again = write(a, resend);
    ASSERT_FALSE(again.empty());
    ASSERT_EQ(read(b, again, resend).size(), 1U);
    const Bytes ack = write(b, resend);  // Chỉ có xác nhận.
    ASSERT_FALSE(ack.empty());
    EXPECT_TRUE(read(a, ack, resend + Duration::milliseconds(10)).empty());
    EXPECT_EQ(a.in_flight(Channel::ReliableUnordered), 0U);
    EXPECT_TRUE(write(a, resend + Duration::seconds(5)).empty());
}

TEST(MessageChannels, AckOnlyPacketsWaitForTheAckDelay) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    ASSERT_TRUE(a.send(Channel::ReliableOrdered, bytes_of("1")).has_value());
    ASSERT_EQ(read(b, write(a, kStart), kStart).size(), 1U);
    ASSERT_FALSE(write(b, kStart).empty());  // Chưa gửi gói nào: xác nhận ngay.
    ASSERT_TRUE(a.send(Channel::ReliableOrdered, bytes_of("2")).has_value());
    ASSERT_EQ(read(b, write(a, kStart), kStart).size(), 1U);
    EXPECT_TRUE(write(b, kStart + (kAckDelay - Duration::nanoseconds(1))).empty());
    EXPECT_FALSE(write(b, kStart + kAckDelay).empty());
    // Gói chỉ có unreliable không đòi xác nhận.
    ASSERT_TRUE(a.send(Channel::Unreliable, bytes_of("u")).has_value());
    ASSERT_EQ(read(b, write(a, kStart), kStart).size(), 1U);
    EXPECT_TRUE(write(b, kStart + Duration::seconds(1)).empty());
}

TEST(MessageChannels, FullReliableWindowPushesBack) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    for (u32 i = 0; i < kReliableWindow; ++i) {
        ASSERT_TRUE(a.send(Channel::ReliableOrdered, pattern(8, i)).has_value());
    }
    EXPECT_EQ(a.send(Channel::ReliableOrdered, bytes_of("thêm")).error().code(),
              ErrorCode::ResourceExhausted);
    EXPECT_EQ(a.in_flight(Channel::ReliableOrdered), kReliableWindow);
    // Kênh không tin cậy và kênh tin cậy kia không bị chặn.
    EXPECT_TRUE(a.send(Channel::ReliableUnordered, bytes_of("khác")).has_value());
    // Một gói chở tối đa 64 tin nhắn: đủ 64 tin nhắn ordered, tin nhắn unordered đợi gói sau.
    const Bytes packet = write(a, kStart);
    ASSERT_EQ(read(b, packet, kStart).size(), kReliableWindow);
    ASSERT_EQ(read(a, write(b, kStart), kStart).size(), 0U);
    EXPECT_EQ(a.in_flight(Channel::ReliableOrdered), 0U);
    EXPECT_EQ(a.in_flight(Channel::ReliableUnordered), 1U);
    EXPECT_TRUE(a.send(Channel::ReliableOrdered, bytes_of("thêm")).has_value());
}

// Bộ đệm gửi là hàng đợi vòng: tin nhắn nằm liền nhau, quay về đầu khi phần cuối thiếu chỗ, và
// đoạn chỉ được dùng lại khi mọi tin nhắn cũ hơn đã được xác nhận. Bên nhận chép thẳng vào đúng
// offset đó.
TEST(MessageChannels, SendBufferIsARingFreedInOrder) {
    MessageChannels a = make_channels(4'096);
    MessageChannels b = make_channels(4'096);
    const auto send = [&](const u32 seed, const usize size) {
        return a.send(Channel::ReliableOrdered, pattern(size, seed));
    };
    ASSERT_TRUE(send(0, 2'000));
    ASSERT_TRUE(send(1, 2'000));
    // 96 byte cuối không đủ, và đầu bộ đệm vẫn là của tin nhắn 0.
    EXPECT_EQ(send(9, 200).error().code(), ErrorCode::ResourceExhausted);
    // Mỗi mảnh gần 1024 byte chiếm một gói: tin nhắn 0 là hai gói đầu.
    std::vector<Bytes> packets;
    for (Bytes packet = write(a, kStart); !packet.empty(); packet = write(a, kStart)) {
        packets.push_back(std::move(packet));
    }
    ASSERT_EQ(packets.size(), 4U);
    std::vector<Bytes> got;
    const auto receive = [&](const Bytes& packet) {
        for (Delivery& delivery : read(b, packet, kStart)) {
            got.push_back(std::move(delivery.data));
        }
    };
    receive(packets[0]);
    receive(packets[1]);
    ASSERT_TRUE(read(a, write(b, kStart), kStart).empty());  // Xác nhận tin nhắn 0.
    EXPECT_EQ(a.in_flight(Channel::ReliableOrdered), 1U);
    // Chỗ của tin nhắn 0 trống: 1 500 byte quay về đầu; sau đó chỉ còn 500 byte trước tin nhắn 1.
    ASSERT_TRUE(send(2, 1'500));
    EXPECT_EQ(send(9, 501).error().code(), ErrorCode::ResourceExhausted);
    ASSERT_TRUE(send(3, 500));
    EXPECT_EQ(send(9, 1).error().code(), ErrorCode::ResourceExhausted);
    receive(packets[2]);
    receive(packets[3]);
    for (Bytes packet = write(a, kStart); !packet.empty(); packet = write(a, kStart)) {
        receive(packet);
    }
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got[0], pattern(2'000, 0));
    EXPECT_EQ(got[1], pattern(2'000, 1));
    EXPECT_EQ(got[2], pattern(1'500, 2));
    EXPECT_EQ(got[3], pattern(500, 3));
    // Xác nhận hết thì bộ đệm trống: tin nhắn chiếm trọn bộ đệm vừa.
    ASSERT_TRUE(read(a, write(b, kStart + kAckDelay), kStart + kAckDelay).empty());
    EXPECT_EQ(a.in_flight(Channel::ReliableOrdered), 0U);
    EXPECT_TRUE(send(4, 4'096));
}

TEST(MessageChannels, RttFollowsTheLink) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    MonoTime now = kStart;
    for (u32 i = 0; i < 64; ++i) {
        ASSERT_TRUE(a.send(Channel::ReliableOrdered, pattern(4, i)).has_value());
        const Bytes out = write(a, now);
        now = now + Duration::milliseconds(20);
        static_cast<void>(read(b, out, now));
        const Bytes back = write(b, now + kAckDelay);
        now = now + kAckDelay + Duration::milliseconds(20);
        static_cast<void>(read(a, back, now));
    }
    // Mỗi vòng: 20 ms đi, 25 ms chờ xác nhận, 20 ms về.
    EXPECT_NEAR(a.rtt().as_seconds_f64(), 0.065, 0.002);
}

// Viết gói bằng tay theo đặc tả để kiểm bên đọc.
struct ManualPacket {
    BitWriter writer;

    explicit ManualPacket(const std::span<std::byte> out, const u16 sequence) : writer(out) {
        writer.write_bits(sequence, 16);
        writer.write_bool(false);
    }
    void reliable(const Channel channel, const u16 id, const u32 count, const u32 index,
                  const std::span<const std::byte> data, const u32 offset = 0) {
        writer.write_bool(true);
        writer.write_bits(std::to_underlying(channel), 2);
        writer.write_bits(id, 16);
        writer.write_ranged(count, 1, kMaxFragments);
        writer.write_ranged(index, 0, kMaxFragments - 1);
        writer.write_bits(offset, 20);
        writer.write_bytes(data, kMaxTransportPayload);
    }
    [[nodiscard]] Bytes finish() {
        writer.write_bool(false);
        EXPECT_FALSE(writer.overflowed());
        return {writer.written().begin(), writer.written().end()};
    }
};

[[nodiscard]] ErrorCode read_error(MessageChannels& channels, const Bytes& packet) {
    const Result<std::span<const ReceivedMessage>> result = channels.read_packet(packet, kStart);
    EXPECT_FALSE(result.has_value());
    return result.has_value() ? ErrorCode::Internal : result.error().code();
}

TEST(MessageChannels, MalformedPacketsAreRejectedWholeAndChangeNothing) {
    MessageChannels b = make_channels(3'000);
    std::array<std::byte, kMaxTransportPayload> buffer{};
    const Bytes one = pattern(kFragmentSize, 1);
    const auto build = [&](auto&& body) {
        ManualPacket packet(buffer, 7);
        body(packet);
        return packet.finish();
    };
    // Id ngoài cửa sổ nhận.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, kReliableWindow, 1, 0,
                                        bytes_of("x"));
                         })),
              ErrorCode::InvalidArgument);
    // Mảnh không phải mảnh cuối mà ngắn hơn 1024 byte.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, 0, 2, 0, bytes_of("ngắn"));
                         })),
              ErrorCode::InvalidArgument);
    // Đoạn vượt bộ đệm nhận (3 000 byte): 3 mảnh là tới 3 072 byte; hay offset đẩy nó ra ngoài.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, 0, 3, 2, one);
                         })),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, 0, 1, 0, bytes_of("x"), 3'000);
                         })),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, 0, 2, 0, one, 1'977);
                         })),
              ErrorCode::InvalidArgument);
    // Chỉ số mảnh không nhỏ hơn số mảnh.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableUnordered, 0, 1, 1, bytes_of("x"));
                         })),
              ErrorCode::InvalidArgument);
    // Tin nhắn hợp lệ đứng trước tin nhắn hỏng cũng không được áp.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, 0, 1, 0, bytes_of("hợp lệ"));
                             p.reliable(Channel::ReliableOrdered, 1, 2, 5, bytes_of("x"));
                         })),
              ErrorCode::InvalidArgument);
    // Hai mảnh của cùng tin nhắn trong một gói khai offset khác nhau.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableUnordered, 0, 2, 1, bytes_of("x"), 0);
                             p.reliable(Channel::ReliableUnordered, 0, 2, 0, one, 1);
                         })),
              ErrorCode::InvalidArgument);
    // Hai mảnh của cùng tin nhắn trong một gói khai số mảnh khác nhau.
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableUnordered, 0, 2, 0, one);
                             p.reliable(Channel::ReliableUnordered, 0, 1, 0, bytes_of("x"));
                         })),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(read_error(b, build([&](ManualPacket& p) {
                             p.reliable(Channel::ReliableOrdered, 0, 2, 1, bytes_of("x"));
                             p.reliable(Channel::ReliableOrdered, 0, 3, 0, one);
                         })),
              ErrorCode::InvalidArgument);
    // Bit đệm khác 0 sau cờ kết thúc.
    Bytes padded = build([&](ManualPacket&) {});
    padded.back() |= std::byte{0x80};
    EXPECT_EQ(read_error(b, padded), ErrorCode::InvalidArgument);
    EXPECT_EQ(read_error(b, {}), ErrorCode::OutOfRange);
    // Không gì ở trên được áp: tin nhắn 0 vẫn chưa giao, và giờ giao được.
    const std::vector<Delivery> got =
        read(b, build([&](ManualPacket& p) {
                 p.reliable(Channel::ReliableOrdered, 0, 1, 0, bytes_of("hợp lệ"));
             }),
             kStart);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_TRUE(std::ranges::equal(got[0].data, bytes_of("hợp lệ")));
}

TEST(MessageChannels, FragmentsOfOneMessageMustAgreeAcrossPackets) {
    MessageChannels b = make_channels(3'000);
    std::array<std::byte, kMaxTransportPayload> buffer{};
    const Bytes one = pattern(kFragmentSize, 4);
    const auto build = [&](const u16 sequence, const u32 count, const u32 index,
                           const std::span<const std::byte> data, const u32 offset) {
        ManualPacket packet(buffer, sequence);
        packet.reliable(Channel::ReliableUnordered, 0, count, index, data, offset);
        return packet.finish();
    };
    // Một span cho cả gói lẫn kỳ vọng: C++ không bảo đảm hai lần viết cùng một chuỗi hằng là cùng
    // một mảng, và bản Debug của MSVC dừng khi iterator của hai span khác nhau được dùng làm một
    // khoảng (CI run 36346903473).
    const std::span<const std::byte> last = bytes_of("cuối");
    EXPECT_TRUE(read(b, build(0, 2, 0, one, 100), kStart).empty());
    EXPECT_EQ(read_error(b, build(1, 2, 1, bytes_of("x"), 0)), ErrorCode::InvalidArgument);
    EXPECT_EQ(read_error(b, build(1, 3, 2, bytes_of("x"), 100)), ErrorCode::InvalidArgument);
    const std::vector<Delivery> got = read(b, build(1, 2, 1, last, 100), kStart);
    ASSERT_EQ(got.size(), 1U);
    Bytes expected = one;
    expected.insert(expected.end(), last.begin(), last.end());
    EXPECT_EQ(got[0].data, expected);
}

TEST(MessageChannels, AtMostSixtyFourMessagesPerPacket) {
    MessageChannels b = make_channels();
    std::array<std::byte, kMaxTransportPayload> buffer{};
    ManualPacket packet(buffer, 0);
    for (u32 i = 0; i <= kMaxMessagesPerPacket; ++i) {
        packet.writer.write_bool(true);
        packet.writer.write_bits(0, 2);
        packet.writer.write_bytes(bytes_of("x"), kMaxTransportPayload);
    }
    EXPECT_EQ(read_error(b, packet.finish()), ErrorCode::InvalidArgument);
}

TEST(MessageChannels, UnreliableDataMustFitItsChannel) {
    MessageChannels b = make_channels();
    std::array<std::byte, kMaxTransportPayload> buffer{};
    const auto unreliable = [&](const Channel channel, const usize size) {
        ManualPacket packet(buffer, 0);
        packet.writer.write_bool(true);
        packet.writer.write_bits(std::to_underlying(channel), 2);
        if (channel == Channel::Sequenced) {
            packet.writer.write_bits(0, 16);
        }
        packet.writer.write_bytes(Bytes(size, std::byte{1}), kMaxTransportPayload);
        return packet.finish();
    };
    EXPECT_EQ(read_error(b, unreliable(Channel::Unreliable, 0)), ErrorCode::InvalidArgument);
    EXPECT_EQ(read_error(b, unreliable(Channel::Sequenced, kMaxUnreliableMessageSize + 1)),
              ErrorCode::InvalidArgument);
    ASSERT_EQ(read(b, unreliable(Channel::Sequenced, kMaxUnreliableMessageSize), kStart).size(),
              1U);
}

// Mọi tiền tố thật sự của một gói hợp lệ thiếu bit ở đâu đó (header, cờ, trường của tin nhắn hay
// dữ liệu), và bị từ chối là OutOfRange.
TEST(MessageChannels, EveryTruncationIsOutOfRange) {
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    // b đã nhận một gói của a, nên gói của b có đủ header xác nhận.
    ASSERT_TRUE(a.send(Channel::Unreliable, bytes_of("chào")));
    ASSERT_EQ(read(b, write(a, kStart), kStart).size(), 1U);
    ASSERT_TRUE(b.send(Channel::Unreliable, bytes_of("một")));
    ASSERT_TRUE(b.send(Channel::Sequenced, bytes_of("hai")));
    ASSERT_TRUE(b.send(Channel::ReliableOrdered, pattern(900, 3)));
    ASSERT_TRUE(b.send(Channel::ReliableUnordered, bytes_of("bốn")));
    const Bytes packet = write(b, kStart);
    ASSERT_FALSE(packet.empty());
    for (usize size = 0; size < packet.size(); ++size) {
        const Bytes prefix(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_EQ(read_error(a, prefix), ErrorCode::OutOfRange) << "cắt còn " << size << " byte";
    }
    EXPECT_EQ(read(a, packet, kStart).size(), 4U);
}

// Pimpl: trạng thái đi theo đối tượng khi chuyển, kể cả tin nhắn đang bay.
TEST(MessageChannels, CanBeMovedWithItsState) {
    MessageChannels a = make_channels();
    ASSERT_TRUE(a.send(Channel::ReliableOrdered, bytes_of("giữ")));
    EXPECT_EQ(a.in_flight(Channel::Unreliable), 0U);  // Chỉ kênh tin cậy có tin nhắn đang bay.
    MessageChannels moved(std::move(a));
    MessageChannels target = make_channels(1);
    target = std::move(moved);
    EXPECT_EQ(target.in_flight(Channel::ReliableOrdered), 1U);
    MessageChannels peer = make_channels();
    const std::vector<Delivery> got = read(peer, write(target, kStart), kStart);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_TRUE(std::ranges::equal(got[0].data, bytes_of("giữ")));
}

// Mạng giả hai chiều: mất 20% gói, trễ ngẫu nhiên 0 tới 100 ms (nên đảo thứ tự), PCG32 seed cố
// định. Mỗi đầu gửi kMessages tin nhắn tin cậy cỡ ngẫu nhiên tới 5 000 byte trên mỗi kênh tin cậy.
class LossyLink {
public:
    static constexpr u32 kMessages = 300;

    // Một nhịp 10 ms: mỗi đầu có thể xếp tin nhắn mới, ghi tới 4 gói, rồi gói tới hạn được giao.
    void tick() {
        now_ = now_ + Duration::milliseconds(10);
        for (u32 side = 0; side < 2; ++side) {
            enqueue(ends_[side]);
            transmit(ends_[side], side == 0);
        }
        deliver_due();
    }

    [[nodiscard]] bool done() const {
        return std::ranges::all_of(ends_, [](const Endpoint& end) {
            return end.sent_ordered.size() == kMessages && end.sent_unordered.size() == kMessages &&
                   end.got_ordered.size() == kMessages && end.got_unordered.size() == kMessages;
        });
    }

    // Tin nhắn của `side` tới đầu kia đúng một lần; kênh ordered đúng thứ tự.
    void expect_delivered(const u32 side) const {
        const Endpoint& sender = ends_[side];
        const Endpoint& receiver = ends_[1 - side];
        EXPECT_EQ(receiver.got_ordered, sender.sent_ordered) << "phía " << side;
        const std::multiset<Bytes> sent(sender.sent_unordered.begin(), sender.sent_unordered.end());
        const std::multiset<Bytes> got(receiver.got_unordered.begin(),
                                       receiver.got_unordered.end());
        EXPECT_EQ(got, sent) << "phía " << side;
    }

private:
    struct Endpoint {
        // 16 KiB: vài tin nhắn lớn đã lấp bộ đệm, nên vòng đệm quay và send bị đẩy lùi thường
        // xuyên.
        MessageChannels channels = make_channels(usize{16} * 1024);
        std::vector<Bytes> sent_ordered;
        std::vector<Bytes> sent_unordered;
        std::vector<Bytes> got_ordered;
        std::vector<Bytes> got_unordered;
    };
    struct InFlight {
        MonoTime arrive;
        u32 order = 0;
        bool to_second = false;
        Bytes bytes;
    };

    void enqueue(Endpoint& end) {
        for (const Channel channel : {Channel::ReliableOrdered, Channel::ReliableUnordered}) {
            std::vector<Bytes>& sent =
                channel == Channel::ReliableOrdered ? end.sent_ordered : end.sent_unordered;
            if (sent.size() == kMessages || rng_.next_below(3) != 0) {
                continue;
            }
            Bytes message = pattern(1 + rng_.next_below(5'000), rng_.next_u32());
            if (end.channels.send(channel, message).has_value()) {
                sent.push_back(std::move(message));
            }
        }
    }

    void transmit(Endpoint& end, const bool to_second) {
        for (u32 budget = 0; budget < 4; ++budget) {
            Bytes packet = write(end.channels, now_);
            if (packet.empty()) {
                return;
            }
            if (rng_.next_below(5) != 0) {  // 20% mất
                network_.push_back({.arrive = now_ + Duration::milliseconds(rng_.next_below(101)),
                                    .order = order_++,
                                    .to_second = to_second,
                                    .bytes = std::move(packet)});
            }
        }
    }

    void deliver_due() {
        std::ranges::sort(network_, [](const InFlight& x, const InFlight& y) {
            return x.arrive < y.arrive || (x.arrive == y.arrive && x.order < y.order);
        });
        while (!network_.empty() && network_.front().arrive <= now_) {
            const InFlight packet = std::move(network_.front());
            network_.erase(network_.begin());
            Endpoint& receiver = ends_[packet.to_second ? 1 : 0];
            for (Delivery& delivery : read(receiver.channels, packet.bytes, now_)) {
                (delivery.channel == Channel::ReliableOrdered ? receiver.got_ordered
                                                              : receiver.got_unordered)
                    .push_back(std::move(delivery.data));
            }
        }
    }

    math::Pcg32 rng_{20'260'927, 11};
    std::array<Endpoint, 2> ends_;
    std::vector<InFlight> network_;
    u32 order_ = 0;
    MonoTime now_ = kStart;
};

TEST(MessageChannels, LossyReorderingLinkDeliversEverythingOnce) {
    LossyLink link;
    for (u32 tick = 0; tick < 20'000 && !link.done(); ++tick) {
        link.tick();
    }
    EXPECT_TRUE(link.done());
    link.expect_delivered(0);
    link.expect_delivered(1);
}

// X.7: sau create, gửi, ghi và đọc gói không cấp phát.
TEST(MessageChannels, SteadyStateDoesNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    MessageChannels a = make_channels();
    MessageChannels b = make_channels();
    const Bytes large = pattern(4'000, 1);
    std::array<std::byte, kMaxTransportPayload> buffer{};
    MonoTime now = kStart;
    const core::testing::AllocationScope scope;
    for (u32 i = 0; i < 200; ++i) {
        now = now + Duration::milliseconds(20);
        ASSERT_TRUE(a.send(Channel::Unreliable, bytes_of("u")).has_value());
        ASSERT_TRUE(a.send(Channel::ReliableOrdered, large).has_value());
        for (usize size = a.write_packet(buffer, now); size != 0;
             size = a.write_packet(buffer, now)) {
            ASSERT_TRUE(b.read_packet(std::span(buffer).first(size), now).has_value());
        }
        for (usize size = b.write_packet(buffer, now); size != 0;
             size = b.write_packet(buffer, now)) {
            ASSERT_TRUE(a.read_packet(std::span(buffer).first(size), now).has_value());
        }
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::net
