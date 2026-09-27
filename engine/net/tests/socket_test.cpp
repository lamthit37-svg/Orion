#include "engine/net/socket.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/address.hpp"
#include "engine/net/detail/native_socket.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::net {
namespace {

using core::Duration;

// Hạn chờ một gói. Loopback thường giao gói ngay; hạn chỉ để test hỏng thay vì treo khi gói không
// tới, không phải một khoảng ngủ (X.4).
constexpr Duration kPatience = Duration::seconds(10);

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text));
}

// Chờ tới khi có gói rồi nhận nó.
[[nodiscard]] Result<std::optional<Datagram>> wait_and_receive(const UdpSocket& socket,
                                                               const std::span<std::byte> buffer) {
    const Result<bool> ready = socket.wait_readable(kPatience);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    return socket.receive(buffer);
}

[[nodiscard]] Address local_address_of(const UdpSocket& socket) {
    const Result<Address> address = socket.local_address();
    EXPECT_TRUE(address.has_value());
    return address.value_or(Address());
}

void expect_round_trip(const Address& loopback) {
    Result<UdpSocket> sender = UdpSocket::open(loopback);
    Result<UdpSocket> receiver = UdpSocket::open(loopback);
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(receiver.has_value());
    const Address to = local_address_of(*receiver);
    EXPECT_NE(to.port(), 0U);
    EXPECT_EQ(to.family(), loopback.family());
    ASSERT_TRUE(sender->send(to, bytes_of("xin chào")).has_value());
    std::array<std::byte, 64> buffer{};
    const Result<std::optional<Datagram>> received = wait_and_receive(*receiver, buffer);
    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(received->has_value());
    const Datagram datagram = received->value_or(Datagram{});
    EXPECT_EQ(datagram.from, local_address_of(*sender));
    const std::span<const std::byte> payload = std::span(buffer).first(datagram.size);
    EXPECT_TRUE(std::ranges::equal(payload, bytes_of("xin chào")));
}

TEST(UdpSocket, RoundTripOnIpv4Loopback) {
    expect_round_trip(Address::loopback_v4(0));
}

TEST(UdpSocket, RoundTripOnIpv6Loopback) {
    const Result<UdpSocket> probe = UdpSocket::open(Address::loopback_v6(0));
    if (!probe.has_value() && probe.error().code() == ErrorCode::Unavailable) {
        GTEST_SKIP() << "máy chạy test không có IPv6 loopback";
    }
    expect_round_trip(Address::loopback_v6(0));
}

TEST(UdpSocket, NothingToReceiveIsNullopt) {
    const Result<UdpSocket> socket = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(socket.has_value());
    std::array<std::byte, 16> buffer{};
    const Result<std::optional<Datagram>> received = socket->receive(buffer);
    ASSERT_TRUE(received.has_value());
    EXPECT_FALSE(received->has_value());
}

TEST(UdpSocket, WaitReadableSeesQueuedDatagrams) {
    Result<UdpSocket> sender = UdpSocket::open(Address::loopback_v4(0));
    Result<UdpSocket> receiver = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(receiver.has_value());
    // Hạn 0 hay âm: chỉ hỏi, không chờ.
    for (const Duration none : {Duration{}, Duration::seconds(-1)}) {
        const Result<bool> empty = receiver->wait_readable(none);
        ASSERT_TRUE(empty.has_value());
        EXPECT_FALSE(*empty);
    }
    ASSERT_TRUE(sender->send(local_address_of(*receiver), bytes_of("có gói")).has_value());
    const Result<bool> ready = receiver->wait_readable(kPatience);
    ASSERT_TRUE(ready.has_value());
    EXPECT_TRUE(*ready);
    // Chờ không lấy gói đi: hỏi lại vẫn thấy, kể cả với hạn lớn nhất (chặn ở INT_MAX mili giây).
    for (const Duration again :
         {Duration{}, Duration::nanoseconds(std::numeric_limits<i64>::max())}) {
        const Result<bool> still = receiver->wait_readable(again);
        ASSERT_TRUE(still.has_value());
        EXPECT_TRUE(*still);
    }
    std::array<std::byte, 16> buffer{};
    const Result<std::optional<Datagram>> received = receiver->receive(buffer);
    ASSERT_TRUE(received.has_value());
    EXPECT_TRUE(received->has_value());
    const Result<bool> drained = receiver->wait_readable(Duration{});
    ASSERT_TRUE(drained.has_value());
    EXPECT_FALSE(*drained);
}

TEST(UdpSocket, OversizedDatagramIsDroppedAsDataLoss) {
    Result<UdpSocket> sender = UdpSocket::open(Address::loopback_v4(0));
    Result<UdpSocket> receiver = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(receiver.has_value());
    const std::vector<std::byte> big(100, std::byte{0x42});
    ASSERT_TRUE(sender->send(local_address_of(*receiver), big).has_value());
    std::array<std::byte, 10> small{};
    const Result<std::optional<Datagram>> received = wait_and_receive(*receiver, small);
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().code(), ErrorCode::DataLoss);
    // Gói đã bị bỏ, không còn phần nào của nó.
    const Result<std::optional<Datagram>> after = receiver->receive(small);
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->has_value());
}

TEST(UdpSocket, PortInUseIsAlreadyExists) {
    const Result<UdpSocket> first = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(first.has_value());
    const Result<UdpSocket> second = UdpSocket::open(local_address_of(*first));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code(), ErrorCode::AlreadyExists);
}

// 192.0.2.1 thuộc TEST-NET-1 (RFC 5737), khối dành cho tài liệu: không máy nào mang địa chỉ này.
TEST(UdpSocket, AddressNotOnThisMachineIsUnavailable) {
    const Result<UdpSocket> socket = UdpSocket::open(Address::v4({192, 0, 2, 1}, 0));
    ASSERT_FALSE(socket.has_value());
    EXPECT_EQ(socket.error().code(), ErrorCode::Unavailable);
}

// Một gói UDP trên IPv4 chở tối đa 65 507 byte.
TEST(UdpSocket, DatagramTooLargeForUdpIsInvalidArgument) {
    const Result<UdpSocket> sender = UdpSocket::open(Address::loopback_v4(0));
    const Result<UdpSocket> receiver = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(receiver.has_value());
    const std::vector<std::byte> huge(70'000, std::byte{0x01});
    const Result<void> sent = sender->send(local_address_of(*receiver), huge);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code(), ErrorCode::InvalidArgument);
}

TEST(UdpSocket, SendToAnotherFamilyIsInvalidArgument) {
    const Result<UdpSocket> socket = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(socket.has_value());
    const Result<void> sent = socket->send(Address::loopback_v6(9), bytes_of("x"));
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code(), ErrorCode::InvalidArgument);
}

// Gửi tới cổng không còn ai nghe: gói ICMP "port unreachable" trả về không được thành lỗi ở lần
// nhận sau (trên Windows phải tắt SIO_UDP_CONNRESET). ICMP có thể tới trước hay sau gói của
// `other`; thứ tự nào thì cả hai lần nhận cũng phải không lỗi.
TEST(UdpSocket, SendingToAClosedPortDoesNotBreakReceiving) {
    const Result<UdpSocket> socket = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(socket.has_value());
    Address closed;
    {
        const Result<UdpSocket> gone = UdpSocket::open(Address::loopback_v4(0));
        ASSERT_TRUE(gone.has_value());
        closed = local_address_of(*gone);
    }
    ASSERT_TRUE(socket->send(closed, bytes_of("không ai nghe")).has_value());

    const Result<UdpSocket> other = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(other.has_value());
    ASSERT_TRUE(other->send(local_address_of(*socket), bytes_of("vẫn nhận")).has_value());
    std::array<std::byte, 64> buffer{};
    const Result<std::optional<Datagram>> received = wait_and_receive(*socket, buffer);
    ASSERT_TRUE(received.has_value()) << received.error().context();
    ASSERT_TRUE(received->has_value());
    EXPECT_EQ(received->value_or(Datagram{}).from, local_address_of(*other));
    const Result<std::optional<Datagram>> after = socket->receive(buffer);
    ASSERT_TRUE(after.has_value()) << after.error().context();
    EXPECT_FALSE(after->has_value());
}

TEST(UdpSocket, MovedSocketKeepsWorkingAndSourceIsEmpty) {
    Result<UdpSocket> opened = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(opened.has_value());
    const Address address = local_address_of(*opened);
    UdpSocket socket = std::move(*opened);
    EXPECT_EQ(local_address_of(socket), address);
    const Result<Address> moved = opened->local_address();
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error().code(), ErrorCode::FailedPrecondition);
    std::array<std::byte, 4> buffer{};
    EXPECT_FALSE(opened->receive(buffer).has_value());
    EXPECT_FALSE(opened->send(address, bytes_of("x")).has_value());
    EXPECT_FALSE(opened->wait_readable(Duration{}).has_value());

    Result<UdpSocket> other = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(other.has_value());
    const Address other_address = local_address_of(*other);
    // Gán đè: socket cũ được đóng, cổng của nó mở lại được.
    socket = std::move(*other);
    EXPECT_EQ(local_address_of(socket), other_address);
    EXPECT_TRUE(UdpSocket::open(address).has_value());
}

// Gửi 100 gói liền rồi mới nhận: không gói nào mất hay tới hai lần trên loopback. Thứ tự tới không
// được kiểm, vì UDP không hứa thứ tự.
TEST(UdpSocket, HundredDatagramsAllArrive) {
    constexpr u32 kCount = 100;
    constexpr std::byte kMarker{0xA5};
    Result<UdpSocket> sender = UdpSocket::open(Address::loopback_v4(0));
    Result<UdpSocket> receiver = UdpSocket::open(Address::loopback_v4(0));
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(receiver.has_value());
    const Address to = local_address_of(*receiver);
    for (u32 i = 0; i < kCount; ++i) {
        const std::array<std::byte, 2> payload{static_cast<std::byte>(i), kMarker};
        ASSERT_TRUE(sender->send(to, payload).has_value());
    }
    std::vector<bool> seen(kCount, false);
    std::array<std::byte, 16> buffer{};
    for (u32 i = 0; i < kCount; ++i) {
        const Result<std::optional<Datagram>> received = wait_and_receive(*receiver, buffer);
        ASSERT_TRUE(received.has_value());
        ASSERT_TRUE(received->has_value()) << "chỉ nhận được " << i << " gói";
        ASSERT_EQ(received->value_or(Datagram{}).size, 2U);
        ASSERT_EQ(buffer[1], kMarker);
        const auto index = std::to_integer<usize>(buffer[0]);
        ASSERT_LT(index, seen.size());
        EXPECT_FALSE(seen[index]) << "gói " << index << " tới hai lần";
        seen[index] = true;
    }
    EXPECT_TRUE(std::ranges::all_of(seen, [](const bool value) { return value; }));
}

TEST(PollTimeout, RoundsUpToMillisecondsAndClampsToInt) {
    constexpr int kMax = std::numeric_limits<int>::max();
    EXPECT_EQ(detail::poll_timeout_ms(Duration{}), 0);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::nanoseconds(-1)), 0);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::nanoseconds(std::numeric_limits<i64>::min())), 0);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::nanoseconds(1)), 1);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::milliseconds(1)), 1);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::milliseconds(1) + Duration::nanoseconds(1)), 2);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::seconds(10)), 10'000);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::milliseconds(kMax)), kMax);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::milliseconds(i64{kMax} + 1)), kMax);
    EXPECT_EQ(detail::poll_timeout_ms(Duration::nanoseconds(std::numeric_limits<i64>::max())),
              kMax);
}

}  // namespace
}  // namespace orion::net
