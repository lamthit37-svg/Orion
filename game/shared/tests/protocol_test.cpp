// Protocol của game: game/shared/protocol/*.schema, sinh bởi tools/codegen (ADR 0004;
// docs/formats/protocol.md). Test giữ hợp đồng của từng tin nhắn (id, kênh, bên gửi); byte trên
// dây của mỗi tin nhắn, tính theo protocol.md mà không qua code của repo; khứ hồi mọi tin nhắn ở
// giá trị biên; độ chính xác của vị trí và hướng; và các giá trị schema cấm.

#include "game/shared/protocol/protocol.hpp"

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/math/random.hpp"
#include "engine/math/scalar.hpp"
#include "engine/net/bitstream.hpp"
#include "engine/net/channels.hpp"
#include "engine/net/rate_limit.hpp"
#include "game/shared/protocol/codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace orion::protocol {
namespace {

using Bytes = std::vector<std::byte>;

constexpr usize kBufferSize = std::max(kMaxMessageSize, kMaxEventSize);
constexpr u64 kMaxDurableId = std::numeric_limits<i64>::max();

template <class Message>
[[nodiscard]] Bytes encode_bytes(const Message& message) {
    std::array<std::byte, kBufferSize> buffer{};
    const Result<usize> size = encode(message, buffer);
    EXPECT_TRUE(size.has_value());
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size.value_or(0))};
}

template <class Message>
[[nodiscard]] ErrorCode encode_error(const Message& message) {
    std::array<std::byte, kBufferSize> buffer{};
    const Result<usize> size = encode(message, buffer);
    EXPECT_FALSE(size.has_value());
    return size.has_value() ? ErrorCode::Internal : size.error().code();
}

[[nodiscard]] Bytes bytes_of(const std::initializer_list<u8> values) {
    Bytes bytes;
    for (const u8 value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

template <class Message, class Group>
[[nodiscard]] Message decoded_as(const Result<Group>& result) {
    if (!result.has_value() || !std::holds_alternative<Message>(*result)) {
        ADD_FAILURE() << "không giải mã ra đúng loại tin nhắn";
        return Message{};
    }
    return std::get<Message>(*result);
}

// Giải mã bằng bộ đọc của đúng bên nhận, trên kênh đã khai.
template <class Message>
[[nodiscard]] Message decode_as(const std::span<const std::byte> wire) {
    if constexpr (requires { Message::kStream; }) {
        return decoded_as<Message>(decode_event(wire));
    } else if constexpr (Message::kSender == Sender::Client) {
        return decoded_as<Message>(decode_client_message(Message::kChannel, wire));
    } else {
        return decoded_as<Message>(decode_server_message(Message::kChannel, wire));
    }
}

// Mã hoá, giải mã, rồi mã hoá lại phải ra đúng từng byte: mỗi tin nhắn chỉ có một cách mã hoá.
template <class Message>
[[nodiscard]] Message round_trip(const Message& message) {
    const Bytes wire = encode_bytes(message);
    const auto decoded = decode_as<Message>(wire);
    EXPECT_EQ(encode_bytes(decoded), wire) << "mã hoá lại ra byte khác";
    return decoded;
}

template <class Message>
void expect_wire(const Message& message, const Bytes& expected) {
    EXPECT_EQ(encode_bytes(message), expected);
    EXPECT_EQ(encode_bytes(decode_as<Message>(expected)), expected);
}

template <class Message>
void expect_contract(const u32 id, const net::Channel channel, const Sender sender) {
    EXPECT_EQ(Message::kId, id);
    EXPECT_EQ(Message::kChannel, channel);
    EXPECT_EQ(Message::kSender, sender);
}

// Id, kênh và bên gửi là hợp đồng giữa client và server: đổi chúng là đổi protocol.
TEST(Protocol, EveryMessageKeepsItsContract) {
    EXPECT_EQ(kProtocolVersion, 1U);
    expect_contract<EnterWorld>(1, net::Channel::ReliableOrdered, Sender::Client);
    expect_contract<LeaveWorld>(2, net::Channel::ReliableOrdered, Sender::Client);
    expect_contract<WorldEntered>(3, net::Channel::ReliableOrdered, Sender::Server);
    expect_contract<EnterWorldFailed>(4, net::Channel::ReliableOrdered, Sender::Server);
    expect_contract<MoveInput>(10, net::Channel::Sequenced, Sender::Client);
    expect_contract<EntityUpdates>(11, net::Channel::Unreliable, Sender::Server);
    expect_contract<EntityRemoved>(12, net::Channel::ReliableOrdered, Sender::Server);
    expect_contract<ChatSend>(20, net::Channel::ReliableOrdered, Sender::Client);
    expect_contract<ChatReceived>(21, net::Channel::ReliableOrdered, Sender::Server);
    EXPECT_EQ(ItemGranted::kId, 1U);
    EXPECT_EQ(ItemGranted::kStream, "economy");
    // Trạng thái thực thể là tin nhắn lớn nhất, và vẫn vừa một tin nhắn unreliable không cắt mảnh.
    static_assert(EntityUpdates::kMaxEncodedSize <= net::kMaxUnreliableMessageSize);
    EXPECT_EQ(kMaxMessageSize, EntityUpdates::kMaxEncodedSize);
    // Server nhận được một MoveInput mỗi tick 50 Hz (ADR 0002) mà không chạm giới hạn tần suất.
    EXPECT_LE(MoveInput::kRateLimit.interval, net::RateLimit::per_second(50, 1).interval);
}

// Byte tính tay theo docs/formats/protocol.md (mục Bitstream, Tin nhắn), không qua code của repo.
// Id message là int[0, 21] trên 5 bit, id event là int[0, 1] trên 1 bit; số ghi bit thấp trước.
TEST(Protocol, WireBytesMatchTheFormat) {
    // id 1; 42 − 1 trên 63 bit.
    const EnterWorld enter{.character_id = 42};
    expect_wire(enter, bytes_of({0x21, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    EXPECT_EQ(decode_as<EnterWorld>(bytes_of({0x21, 0x05, 0, 0, 0, 0, 0, 0, 0})), enter);
    expect_wire(LeaveWorld{}, bytes_of({0x02}));
    // id 3; tick 64 bit; id nhân bản 32 bit.
    const WorldEntered entered{.tick = Tick{0x0102'0304'0506'0708},
                               .self_id = ReplicatedId{0x0A0B'0C0D}};
    expect_wire(entered, bytes_of({0x03, 0xE1, 0xC0, 0xA0, 0x80, 0x60, 0x40, 0x20, 0xA0, 0x81, 0x61,
                                   0x41, 0x01}));
    // id 4; lý do 2 − 1 trên 2 bit.
    expect_wire(EnterWorldFailed{.reason = EnterWorldError::CharacterInWorld}, bytes_of({0x24}));
    // id 10; tick 16 bit; 1 khung trên 4 bit; 100 và −100 là 200 và 0 trên 8 bit; yaw chỉ số 0
    // trên 13 bit; jump.
    MoveInput input{.tick = ShortTick{0xBEEF}};
    ASSERT_TRUE(
        input.frames.push_back(MoveFrame{.move_x = 100, .move_z = -100, .yaw = 0.0, .jump = true})
            .has_value());
    expect_wire(input, bytes_of({0xEA, 0xDD, 0x37, 0x90, 0x01, 0x00, 0x40}));
    // id 11; tick; 1 thực thể trên 7 bit; id 7; vị trí (1,5; −2,25; 100) là chỉ số 3 276 950,
    // 204 575, 3 286 800; yaw 3 là chỉ số 3 000.
    EntityUpdates updates{.tick = ShortTick{0x1234}};
    ASSERT_TRUE(
        updates.entities
            .push_back(EntityState{
                .id = ReplicatedId{7}, .position = {.x = 1.5, .y = -2.25, .z = 100.0}, .yaw = 3.0})
            .has_value());
    expect_wire(updates, bytes_of({0x8B, 0x46, 0x22, 0x70, 0x00, 0x00, 0x00, 0x60, 0x09, 0x20, 0xFB,
                                   0xF8, 0x18, 0xC4, 0x89, 0x0C, 0x77, 0x01}));
    // id 20; Zone là 1 trên 1 bit; độ dài 2 trên 8 bit; "hi".
    ChatSend chat{.scope = ChatScope::Zone};
    ASSERT_TRUE(chat.text.assign("hi").has_value());
    expect_wire(chat, bytes_of({0xB4, 0x00, 0x5A, 0x1A}));
    // Event id 1 trên 1 bit; 1 − 1 và 2 − 1 trên 63 bit; 3 − 1 trên 32 bit; 4 − 1 trên 20 bit.
    const ItemGranted granted{.grant_id = 1, .character_id = 2, .item_def = 3, .count = 4};
    expect_wire(granted,
                bytes_of({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00}));
    EXPECT_EQ(round_trip(granted), granted);
}

[[nodiscard]] EntityUpdates full_updates() {
    const net::Quantization horizontal(-32768.0, 32768.0, 0.01);
    const net::Quantization vertical(-2048.0, 2048.0, 0.01);
    const net::Quantization yaw(0.0, 6.2832, 0.001);
    EntityUpdates updates{.tick = ShortTick{0xFFFF}};
    for (u32 i = 0; i < 64; ++i) {
        // Toạ độ nằm đúng trên lưới lượng tử hoá, từ biên này sang biên kia.
        const EntityState state{
            .id = ReplicatedId{std::numeric_limits<u32>::max() - i},
            .position = {.x = horizontal.value_of(i * (horizontal.max_index() / 63)),
                         .y = vertical.value_of(vertical.max_index() - i),
                         .z = horizontal.value_of(horizontal.max_index() - (u64{i} * 1'000))},
            .yaw = yaw.value_of(yaw.max_index() - i)};
        EXPECT_TRUE(updates.entities.push_back(state).has_value());
    }
    return updates;
}

[[nodiscard]] std::string repeated(const std::string& text, const usize count) {
    std::string out;
    for (usize i = 0; i < count; ++i) {
        out += text;
    }
    return out;
}

// Mọi tin nhắn ở giá trị biên: số lớn nhất, mảng đầy, chuỗi dài nhất; tin nhắn dài nhất có đúng
// cỡ kMaxEncodedSize mà codegen tính.
TEST(Protocol, EveryMessageRoundTripsAtItsLimits) {
    const EnterWorld enter{.character_id = kMaxDurableId};
    EXPECT_EQ(round_trip(enter), enter);
    EXPECT_EQ(round_trip(LeaveWorld{}), LeaveWorld{});
    const WorldEntered entered{.tick = Tick{std::numeric_limits<u64>::max()},
                               .self_id = ReplicatedId{std::numeric_limits<u32>::max()}};
    EXPECT_EQ(round_trip(entered), entered);
    for (const EnterWorldError reason :
         {EnterWorldError::CharacterNotFound, EnterWorldError::CharacterInWorld,
          EnterWorldError::ZoneUnavailable}) {
        EXPECT_EQ(round_trip(EnterWorldFailed{.reason = reason}).reason, reason);
    }

    MoveInput input{.tick = ShortTick{0}};
    for (i32 i = 0; i < 8; ++i) {
        const MoveFrame frame{.move_x = static_cast<i8>(100 - (i * 25)),
                              .move_z = static_cast<i8>(-100 + (i * 25)),
                              .yaw = net::Quantization(0.0, 6.2832, 0.001).value_of(6284),
                              .jump = i % 2 == 0};
        ASSERT_TRUE(input.frames.push_back(frame).has_value());
    }
    EXPECT_EQ(round_trip(input), input);
    EXPECT_EQ(encode_bytes(input).size(), MoveInput::kMaxEncodedSize);

    const EntityUpdates updates = full_updates();
    EXPECT_EQ(round_trip(updates), updates);
    EXPECT_EQ(encode_bytes(updates).size(), EntityUpdates::kMaxEncodedSize);

    EntityRemoved removed{.tick = ShortTick{0x8000}};
    for (u32 i = 0; i < 64; ++i) {
        ASSERT_TRUE(removed.ids.push_back(ReplicatedId{(i * 65'537) + 1}).has_value());
    }
    EXPECT_EQ(round_trip(removed), removed);
    EXPECT_EQ(encode_bytes(removed).size(), EntityRemoved::kMaxEncodedSize);

    ChatSend chat{.scope = ChatScope::Say};
    ASSERT_TRUE(chat.text.assign(repeated("chào ", 42) + "ạ").has_value());  // 252 + 3 byte.
    ASSERT_EQ(chat.text.size(), 255U);
    EXPECT_EQ(round_trip(chat), chat);
    EXPECT_EQ(encode_bytes(chat).size(), ChatSend::kMaxEncodedSize);

    ChatReceived received{.scope = ChatScope::Zone, .sender = ReplicatedId{12}};
    ASSERT_TRUE(received.sender_name.assign(repeated("ệ", 16)).has_value());  // 16 × 3 byte.
    ASSERT_TRUE(received.text.assign(std::string(255, '!')).has_value());
    EXPECT_EQ(round_trip(received), received);
    EXPECT_EQ(encode_bytes(received).size(), ChatReceived::kMaxEncodedSize);

    const ItemGranted granted{.grant_id = kMaxDurableId,
                              .character_id = kMaxDurableId,
                              .item_def = std::numeric_limits<u32>::max(),
                              .count = 1'000'000};
    EXPECT_EQ(round_trip(granted), granted);
    EXPECT_EQ(encode_bytes(granted).size(), ItemGranted::kMaxEncodedSize);
}

// Vị trí giữ 1 cm và hướng 0,001 rad (ADR 0001 mục 8): sau khứ hồi, sai số không quá nửa bước.
// Vị trí ngoài thế giới bị kẹp vào biên.
TEST(Protocol, PositionsAndYawKeepTheirPrecision) {
    constexpr f64 kSlack = 1e-9;  // sai số làm tròn của min + i · step, cỡ 1e-11 ở biên
    math::Pcg32 rng(20'260'927, 11);
    EntityUpdates updates;
    for (u32 round = 0; round < 200; ++round) {
        updates.entities.clear();
        for (u32 i = 0; i < 64; ++i) {
            const EntityState state{.id = ReplicatedId{i + 1},
                                    .position = {.x = (rng.next_f64() * 65'536.0) - 32'768.0,
                                                 .y = (rng.next_f64() * 4'096.0) - 2'048.0,
                                                 .z = (rng.next_f64() * 65'536.0) - 32'768.0},
                                    .yaw = rng.next_f64() * math::kTau};
            ASSERT_TRUE(updates.entities.push_back(state).has_value());
        }
        const EntityUpdates decoded = round_trip(updates);
        ASSERT_EQ(decoded.entities.size(), 64U) << "vòng " << round;
        for (u32 i = 0; i < 64; ++i) {
            const EntityState& sent = updates.entities[i];
            const EntityState& got = decoded.entities[i];
            EXPECT_LE(math::abs(got.position.x - sent.position.x), 0.005 + kSlack);
            EXPECT_LE(math::abs(got.position.y - sent.position.y), 0.005 + kSlack);
            EXPECT_LE(math::abs(got.position.z - sent.position.z), 0.005 + kSlack);
            EXPECT_LE(math::abs(got.yaw - sent.yaw), 0.0005 + kSlack);
        }
    }

    updates.entities.clear();
    ASSERT_TRUE(updates.entities
                    .push_back(EntityState{.id = ReplicatedId{1},
                                           .position = {.x = 40'000.0, .y = -5'000.0, .z = -1e9},
                                           .yaw = 7.0})
                    .has_value());
    const EntityState clamped = round_trip(updates).entities[0];
    EXPECT_EQ(clamped.position.x, 32'768.0);
    EXPECT_EQ(clamped.position.y, -2'048.0);
    EXPECT_EQ(clamped.position.z, -32'768.0);
    EXPECT_EQ(clamped.yaw, 6.2832);
}

// Tick rút gọn của tin nhắn thường được bên nhận khôi phục theo tick nó đã biết (ADR 0002 mục 2).
TEST(Protocol, ShortTicksExpandAroundTheReceiverTick) {
    constexpr Tick kZoneTick{3'000'000'017};
    const EntityUpdates decoded = round_trip(EntityUpdates{.tick = shorten(kZoneTick)});
    // Bên nhận lệch tới 2^15 − 1 tick về hai phía, tức hơn 10 phút ở 50 Hz, vẫn khôi phục đúng.
    EXPECT_EQ(expand(decoded.tick, Tick{kZoneTick.value + 32'767}), kZoneTick);
    EXPECT_EQ(expand(decoded.tick, Tick{kZoneTick.value - 32'767}), kZoneTick);
    const MoveInput input = round_trip(MoveInput{.tick = shorten(kZoneTick)});
    EXPECT_EQ(expand(input.tick, kZoneTick), kZoneTick);
}

TEST(Protocol, EncodersRejectValuesTheSchemaForbids) {
    // Id bền 0 không hợp lệ (ADR 0006); id từ 2^63 không có trong cột BIGINT.
    EXPECT_EQ(encode_error(EnterWorld{.character_id = 0}), ErrorCode::InvalidArgument);
    EXPECT_EQ(encode_error(EnterWorld{.character_id = kMaxDurableId + 1}),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(encode_error(ItemGranted{.grant_id = 0}), ErrorCode::InvalidArgument);
    EXPECT_EQ(encode_error(ItemGranted{.count = 1'000'001}), ErrorCode::InvalidArgument);
    MoveInput input;
    ASSERT_TRUE(input.frames.push_back(MoveFrame{.move_x = 101}).has_value());
    EXPECT_EQ(encode_error(input), ErrorCode::InvalidArgument);
    // Giá trị enum không khai trong schema.
    ChatSend chat;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): cố ý tạo giá trị không khai.
    chat.scope = static_cast<ChatScope>(2);
    EXPECT_EQ(encode_error(chat), ErrorCode::InvalidArgument);
    EnterWorldFailed failed;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): cố ý tạo giá trị không khai.
    failed.reason = static_cast<EnterWorldError>(0);
    EXPECT_EQ(encode_error(failed), ErrorCode::InvalidArgument);
    // Mặc định của mọi tin nhắn nằm trong khoảng đã khai nên mã hoá được.
    EXPECT_EQ(round_trip(ItemGranted{}), ItemGranted{});
    EXPECT_EQ(round_trip(EnterWorldFailed{}), EnterWorldFailed{});
}

template <class Group>
[[nodiscard]] ErrorCode decode_error(const Result<Group>& result) {
    EXPECT_FALSE(result.has_value());
    return result.has_value() ? ErrorCode::Internal : result.error().code();
}

// Bảng lỗi của protocol.md, mục Tin nhắn, trên các tin nhắn của game.
TEST(Protocol, DecodersRejectWhatTheSchemaForbids) {
    const Bytes chat = encode_bytes(ChatSend{.scope = ChatScope::Say});
    EXPECT_EQ(decode_error(decode_client_message(net::Channel::Unreliable, chat)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_error(decode_server_message(net::Channel::ReliableOrdered, chat)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_error(decode_client_message(net::Channel::ReliableOrdered,
                                                 encode_bytes(ChatReceived{}))),
              ErrorCode::InvalidArgument);

    std::array<std::byte, kBufferSize> buffer{};
    // Chat không phải UTF-8: id 20, Say, độ dài 1, byte 0xFF.
    net::BitWriter text(buffer);
    text.write_bits(20, 5);
    text.write_bits(0, 1);
    text.write_bits(1, 8);
    text.write_bits(0xFF, 8);
    EXPECT_EQ(decode_error(decode_client_message(net::Channel::ReliableOrdered, text.written())),
              ErrorCode::InvalidArgument);
    // character_id thô 2^63 − 1, tức giá trị 2^63: vượt khoảng.
    net::BitWriter id(buffer);
    id.write_bits(1, 5);
    id.write_bits(0x7FFF'FFFF'FFFF'FFFF, 63);
    EXPECT_EQ(decode_error(decode_client_message(net::Channel::ReliableOrdered, id.written())),
              ErrorCode::InvalidArgument);
    // EnterWorldFailed: lý do thô 3 trên 2 bit, tức 4, vượt int[1, 3].
    EXPECT_EQ(decode_error(decode_server_message(net::Channel::ReliableOrdered, bytes_of({0x64}))),
              ErrorCode::InvalidArgument);
    // Id 0 không khai, ở cả message lẫn event; id 22 tới 31 vượt id lớn nhất 21.
    EXPECT_EQ(decode_error(decode_client_message(net::Channel::ReliableOrdered, bytes_of({0x00}))),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_error(decode_server_message(net::Channel::Unreliable, bytes_of({0x1F}))),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_error(decode_event(bytes_of({0x00}))), ErrorCode::InvalidArgument);
    // Thiếu byte.
    EXPECT_EQ(decode_error(decode_event({})), ErrorCode::OutOfRange);
    const Bytes enter = encode_bytes(EnterWorld{.character_id = 5});
    EXPECT_EQ(decode_error(decode_client_message(net::Channel::ReliableOrdered,
                                                 std::span(enter).first(enter.size() - 1))),
              ErrorCode::OutOfRange);
}

// X.7: ghi và đọc tin nhắn lớn nhất mỗi tick không cấp phát.
TEST(Protocol, EncodeAndDecodeDoNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    const EntityUpdates updates = full_updates();
    std::array<std::byte, kBufferSize> buffer{};
    const core::testing::AllocationScope scope;
    for (u32 i = 0; i < 50; ++i) {
        const usize size = encode(updates, buffer).value_or(0);
        const Result<ServerMessage> decoded = decode_server_message(
            net::Channel::Unreliable, std::span<const std::byte>(buffer).first(size));
        ASSERT_TRUE(decoded.has_value());
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::protocol
