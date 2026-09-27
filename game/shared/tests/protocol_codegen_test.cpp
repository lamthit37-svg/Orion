// Code do tools/codegen sinh từ protocol thử game/shared/tests/protocol/everything.schema, schema
// dùng mọi kiểu và mọi thuộc tính của docs/formats/protocol.md. Test khứ hồi mã hoá rồi giải mã
// (ADR 0004), cỡ lớn nhất đúng như codegen tính, và mọi lỗi của bảng Tin nhắn.

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/net/bitstream.hpp"
#include "engine/net/channels.hpp"
#include "engine/net/rate_limit.hpp"
#include "game/shared/protocol/codec.hpp"
#include "game/shared/tests/generated/test_protocol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orion::protocol::testing {
namespace {

using Bytes = std::vector<std::byte>;

template <class Message>
[[nodiscard]] Bytes encode_bytes(const Message& message) {
    std::array<std::byte, kMaxMessageSize> buffer{};
    const Result<usize> size = encode(message, buffer);
    EXPECT_TRUE(size.has_value());
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size.value_or(0))};
}

// Mọi trường đều dài nhất và xa nhất có thể; quantized nằm đúng trên lưới nên khứ hồi được đúng
// từng bit.
[[nodiscard]] Everything full_everything() {
    const net::Quantization angle(0.0, 6.2832, 0.001);
    const net::Quantization coordinate(-100.0, 100.0, 0.5);
    Everything message;
    message.flag = true;
    message.small_bits = 7;
    message.full_bits = std::numeric_limits<u64>::max();
    message.signed_value = -1'000;
    message.unsigned_value = 4'000'000'000;
    message.extreme = std::numeric_limits<i64>::min();
    message.angle = angle.value_of(angle.max_index());
    message.altitude = 20.0;
    EXPECT_TRUE(message.blob.assign(Bytes(32, std::byte{0xAB})).has_value());
    EXPECT_TRUE(message.name.assign("Lý Thái Tổ Hà Nội").has_value());  // Đúng 24 byte UTF-8.
    message.now = Tick{0xFFFF'FFFF'FFFF'FFFF};
    message.recent = ShortTick{0xFFFF};
    message.target = ReplicatedId{0xFFFF'FFFF};
    message.wide = Wide::Huge;
    message.shape.color = Color::Blue;
    for (u32 i = 0; i < 8; ++i) {
        const Point point{.x = coordinate.value_of(i), .y = coordinate.value_of(400 - i)};
        EXPECT_TRUE(message.shape.points.push_back(point).has_value());
    }
    EXPECT_TRUE(message.shape.label.assign("tam giác đều").has_value());  // Đúng 16 byte.
    for (u8 row = 0; row < 3; ++row) {
        BoundedArray<u8, 2> cells;
        EXPECT_TRUE(cells.push_back(row).has_value());
        EXPECT_TRUE(cells.push_back(9).has_value());
        EXPECT_TRUE(message.grid.push_back(cells).has_value());
    }
    for (const bool flag : {true, false, true, true}) {
        EXPECT_TRUE(message.flags.push_back(flag).has_value());
    }
    return message;
}

TEST(GeneratedProtocol, ConstantsFollowTheSchema) {
    EXPECT_EQ(kProtocolVersion, 7U);
    EXPECT_EQ(Everything::kId, 2U);
    EXPECT_EQ(Everything::kChannel, net::Channel::ReliableOrdered);
    EXPECT_EQ(Everything::kSender, Sender::Client);
    EXPECT_EQ(Everything::kRateLimit.interval, net::RateLimit::per_second(5, 5).interval);
    EXPECT_EQ(Everything::kRateLimit.burst, 5U);
    EXPECT_EQ(Status::kChannel, net::Channel::Sequenced);
    EXPECT_EQ(Status::kSender, Sender::Server);
    EXPECT_EQ(Notice::kId, 40U);
    EXPECT_EQ(Granted::kStream, "economy");
    EXPECT_EQ(Audit::kStream, "audit");
    EXPECT_EQ(Ping::kMaxEncodedSize, 1U);  // Chỉ có id: 6 bit cho id tới 40.
}

// Tin nhắn dài nhất có cỡ đúng bằng kMaxEncodedSize mà codegen tính.
TEST(GeneratedProtocol, LargestMessageHasExactlyTheComputedSize) {
    EXPECT_EQ(encode_bytes(full_everything()).size(), Everything::kMaxEncodedSize);
    Notice notice;
    ASSERT_TRUE(notice.text.assign(std::string(100, 'x')).has_value());
    EXPECT_EQ(encode_bytes(notice).size(), Notice::kMaxEncodedSize);
}

TEST(GeneratedProtocol, ClientMessagesRoundTrip) {
    const Everything original = full_everything();
    const Bytes wire = encode_bytes(original);
    const Result<ClientMessage> decoded =
        decode_client_message(net::Channel::ReliableOrdered, wire);
    ASSERT_TRUE(decoded.has_value());
    const auto* everything = std::get_if<Everything>(&*decoded);
    ASSERT_NE(everything, nullptr);
    EXPECT_EQ(*everything, original);
    // Mã hoá là chính tắc: mã lại cho đúng từng byte.
    EXPECT_EQ(encode_bytes(*everything), wire);

    // Mặc định nằm trong khoảng đã khai: 0, hay min khi 0 ngoài khoảng; enum lấy giá trị khai đầu.
    const Everything defaults;
    EXPECT_EQ(defaults.altitude, 10.0);
    EXPECT_EQ(defaults.wide, Wide::Small);
    const Result<ClientMessage> small =
        decode_client_message(net::Channel::ReliableOrdered, encode_bytes(defaults));
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(std::get<Everything>(*small), defaults);

    const Result<ClientMessage> ping =
        decode_client_message(net::Channel::Unreliable, encode_bytes(Ping{}));
    ASSERT_TRUE(ping.has_value());
    EXPECT_TRUE(std::holds_alternative<Ping>(*ping));
}

TEST(GeneratedProtocol, ServerMessagesAndEventsRoundTrip) {
    Status status;
    status.color = Color::Green;
    status.position = Point{.x = -100.0, .y = 99.5};
    const Result<ServerMessage> decoded =
        decode_server_message(net::Channel::Sequenced, encode_bytes(status));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(std::get<Status>(*decoded), status);

    Notice notice;
    ASSERT_TRUE(notice.text.assign("bảo trì lúc 3 giờ").has_value());
    const Result<ServerMessage> notice_decoded =
        decode_server_message(net::Channel::ReliableUnordered, encode_bytes(notice));
    ASSERT_TRUE(notice_decoded.has_value());
    EXPECT_EQ(std::get<Notice>(*notice_decoded), notice);

    const Result<Event> granted = decode_event(encode_bytes(Granted{.amount = 1'000}));
    ASSERT_TRUE(granted.has_value());
    EXPECT_EQ(std::get<Granted>(*granted).amount, 1'000U);
    const Result<Event> audit = decode_event(encode_bytes(Audit{}));
    ASSERT_TRUE(audit.has_value());
    EXPECT_TRUE(std::holds_alternative<Audit>(*audit));
}

[[nodiscard]] ErrorCode client_error(const net::Channel channel,
                                     const std::span<const std::byte> bytes) {
    const Result<ClientMessage> result = decode_client_message(channel, bytes);
    EXPECT_FALSE(result.has_value());
    return result.has_value() ? ErrorCode::Internal : result.error().code();
}

// Bảng lỗi của protocol.md, mục Tin nhắn.
TEST(GeneratedProtocol, DecodersRejectWrongDirectionChannelAndIds) {
    const Bytes everything = encode_bytes(full_everything());
    // Sai kênh.
    EXPECT_EQ(client_error(net::Channel::Unreliable, everything), ErrorCode::InvalidArgument);
    // Message của server gửi tới bên chờ message của client, và ngược lại.
    EXPECT_EQ(client_error(net::Channel::Sequenced, encode_bytes(Status{})),
              ErrorCode::InvalidArgument);
    const Result<ServerMessage> wrong_side =
        decode_server_message(net::Channel::ReliableOrdered, everything);
    ASSERT_FALSE(wrong_side.has_value());
    EXPECT_EQ(wrong_side.error().code(), ErrorCode::InvalidArgument);
    // Id không khai: 0 và 5 (6 bit, id lớn nhất 40).
    const std::array zero = {std::byte{0x00}};
    const std::array five = {std::byte{0x05}};
    EXPECT_EQ(client_error(net::Channel::Unreliable, zero), ErrorCode::InvalidArgument);
    EXPECT_EQ(client_error(net::Channel::Unreliable, five), ErrorCode::InvalidArgument);
    // Id vượt 40 trên 6 bit.
    const std::array over = {std::byte{0x3F}};
    EXPECT_EQ(client_error(net::Channel::Unreliable, over), ErrorCode::InvalidArgument);
    const std::array event_id = {std::byte{0x03}};
    const Result<Event> unknown_event = decode_event(event_id);
    ASSERT_FALSE(unknown_event.has_value());
    EXPECT_EQ(unknown_event.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(client_error(net::Channel::Unreliable, {}), ErrorCode::OutOfRange);
}

// Mọi tiền tố thật sự của một tin nhắn hợp lệ bị từ chối, và byte thừa phía sau cũng vậy.
TEST(GeneratedProtocol, EveryTruncationAndTrailingByteIsRejected) {
    const Bytes wire = encode_bytes(full_everything());
    for (usize size = 0; size < wire.size(); ++size) {
        const ErrorCode code = client_error(net::Channel::ReliableOrdered,
                                            std::span<const std::byte>(wire).first(size));
        EXPECT_TRUE(code == ErrorCode::OutOfRange || code == ErrorCode::InvalidArgument)
            << "cắt còn " << size << " byte";
    }
    Bytes longer = wire;
    longer.push_back(std::byte{0});
    EXPECT_EQ(client_error(net::Channel::ReliableOrdered, longer), ErrorCode::InvalidArgument);
}

// Giá trị enum không có trong schema: bên ghi từ chối, bên đọc cũng vậy khi nó đến từ mạng.
TEST(GeneratedProtocol, UndeclaredEnumValuesAreRejected) {
    Status status;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): cố ý tạo giá trị không khai.
    status.color = static_cast<Color>(3);
    std::array<std::byte, kMaxMessageSize> buffer{};
    const Result<usize> encoded = encode(status, buffer);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code(), ErrorCode::InvalidArgument);

    // Status trên dây: id 3 (6 bit), Color 3 (3 bit, trong [0, 7] nhưng không khai), hai toạ độ.
    net::BitWriter writer(buffer);
    writer.write_bits(3, 6);
    writer.write_bits(3, 3);
    writer.write_bits(0, 9);
    writer.write_bits(0, 9);
    const Result<ServerMessage> decoded =
        decode_server_message(net::Channel::Sequenced, writer.written());
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), ErrorCode::InvalidArgument);
}

TEST(GeneratedProtocol, EncoderRejectsOutOfRangeFieldsAndSmallBuffers) {
    std::array<std::byte, kMaxMessageSize> buffer{};
    Everything bad_int = full_everything();
    bad_int.signed_value = 1'001;
    EXPECT_EQ(encode(bad_int, buffer).error().code(), ErrorCode::InvalidArgument);
    Everything bad_bits = full_everything();
    bad_bits.small_bits = 8;
    EXPECT_EQ(encode(bad_bits, buffer).error().code(), ErrorCode::InvalidArgument);
    Everything bad_wide = full_everything();
    bad_wide.wide = static_cast<Wide>(2);
    EXPECT_EQ(encode(bad_wide, buffer).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(encode(Granted{.amount = 0}, buffer).error().code(), ErrorCode::InvalidArgument);
    std::array<std::byte, 16> small{};
    EXPECT_EQ(encode(full_everything(), small).error().code(), ErrorCode::ResourceExhausted);
}

// X.7: ghi và đọc tin nhắn không cấp phát.
TEST(GeneratedProtocol, EncodeAndDecodeDoNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    const Everything message = full_everything();
    std::array<std::byte, kMaxMessageSize> buffer{};
    const core::testing::AllocationScope scope;
    for (u32 i = 0; i < 50; ++i) {
        const usize size = encode(message, buffer).value_or(0);
        const Result<ClientMessage> decoded = decode_client_message(
            net::Channel::ReliableOrdered, std::span<const std::byte>(buffer).first(size));
        ASSERT_TRUE(decoded.has_value());
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::protocol::testing
