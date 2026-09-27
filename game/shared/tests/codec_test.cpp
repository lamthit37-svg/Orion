// Code hỗ trợ của protocol sinh ra (game/shared/protocol/codec.hpp; docs/formats/protocol.md). PRNG
// seed cố định (X.4).

#include "game/shared/protocol/codec.hpp"

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/math/random.hpp"
#include "engine/net/bitstream.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

namespace orion::protocol {
namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text));
}

TEST(Tick, ShortTickKeepsTheLow16Bits) {
    EXPECT_EQ(shorten(Tick{0x1234'5678'9ABC}).value, 0x9ABCU);
    EXPECT_EQ(shorten(Tick{65'536}).value, 0U);
}

TEST(Tick, ExpandPicksTheNearestTickWithTheSameLowBits) {
    // Qua mốc quay vòng theo cả hai chiều.
    EXPECT_EQ(expand(shorten(Tick{65'540}), Tick{65'530}), Tick{65'540});
    EXPECT_EQ(expand(shorten(Tick{65'530}), Tick{65'540}), Tick{65'530});
    EXPECT_EQ(expand(shorten(Tick{70'000}), Tick{70'000}), Tick{70'000});
    // Cách đúng 2^15: lấy phía trước.
    EXPECT_EQ(expand(ShortTick{0x8000}, Tick{0x10000}), Tick{0x18000});
    EXPECT_EQ(expand(ShortTick{0x7FFF}, Tick{0x10000}), Tick{0x17FFF});
    EXPECT_EQ(expand(ShortTick{0x8001}, Tick{0x10000}), Tick{0x8001});
    // Gần 0 không có tick âm: lấy phía trước.
    EXPECT_EQ(expand(ShortTick{0xFFFF}, Tick{5}), Tick{0xFFFF});
}

// Mọi tick cách tham chiếu dưới 2^15 được khôi phục đúng.
TEST(Tick, ExpandRoundTripsWithinHalfTheRange) {
    math::Pcg32 random(2'026, 7);
    for (u32 i = 0; i < 100'000; ++i) {
        const u64 reference = (u64{random.next_u32()} << 8U) + random.next_below(1'000);
        const i64 offset = static_cast<i64>(random.next_below(65'535)) - 32'767;
        const u64 tick = static_cast<u64>(static_cast<i64>(reference) + offset);
        ASSERT_EQ(expand(shorten(Tick{tick}), Tick{reference}), Tick{tick})
            << "tham chiếu " << reference << ", lệch " << offset;
    }
}

TEST(ReplicatedId, ZeroIsNoEntity) {
    EXPECT_FALSE(ReplicatedId{}.valid());
    EXPECT_TRUE(ReplicatedId{7}.valid());
}

TEST(BoundedBytes, AssignChecksTheCapacity) {
    BoundedBytes<4> bytes;
    ASSERT_TRUE(bytes.assign(bytes_of("abcd")).has_value());
    EXPECT_EQ(bytes.size(), 4U);
    const Result<void> too_long = bytes.assign(bytes_of("abcde"));
    ASSERT_FALSE(too_long.has_value());
    EXPECT_EQ(too_long.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(std::ranges::equal(bytes.view(), bytes_of("abcd")));  // Lỗi không đổi gì.
    BoundedBytes<4> other;
    ASSERT_TRUE(other.assign(bytes_of("abcd")).has_value());
    EXPECT_EQ(bytes, other);
    ASSERT_TRUE(other.assign(bytes_of("ab")).has_value());
    EXPECT_NE(bytes, other);
}

TEST(BoundedString, AssignChecksTheCapacityAndUtf8) {
    BoundedString<8> text;
    ASSERT_TRUE(text.assign("chào").has_value());  // 5 byte UTF-8.
    EXPECT_EQ(text.view(), "chào");
    EXPECT_EQ(text.size(), 5U);
    EXPECT_EQ(text.assign("chào chào").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(text.assign("\xC3").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(text.view(), "chào");
    BoundedString<8> same;
    ASSERT_TRUE(same.assign("chào").has_value());
    EXPECT_EQ(text, same);
}

TEST(BoundedArray, PushBackStopsAtTheCapacity) {
    BoundedArray<u16, 2> values;
    EXPECT_TRUE(values.empty());
    ASSERT_TRUE(values.push_back(7).has_value());
    ASSERT_TRUE(values.push_back(9).has_value());
    const Result<void> full = values.push_back(11);
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error().code(), ErrorCode::ResourceExhausted);
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values[0], 7U);
    EXPECT_EQ(values[1], 9U);
    values[1] = 10;
    u32 sum = 0;
    for (const u16 value : values) {
        sum += value;
    }
    EXPECT_EQ(sum, 17U);
    values.resize(1);
    values.resize(2);  // Phần tử mới là T{}.
    EXPECT_EQ(values[1], 0U);
    BoundedArray<u16, 2> other;
    ASSERT_TRUE(other.push_back(7).has_value());
    ASSERT_TRUE(other.push_back(0).has_value());
    EXPECT_EQ(values, other);
    values.clear();
    EXPECT_TRUE(values.empty());
    EXPECT_NE(values, other);
}

// Mọi loại trường ghi rồi đọc lại đúng, và bên đọc kết thúc sạch.
TEST(Codec, EveryFieldKindRoundTrips) {
    const net::Quantization yaw(0.0, 6.2832, 0.01);
    BoundedBytes<16> key;
    ASSERT_TRUE(key.assign(bytes_of("khoá")).has_value());
    BoundedString<32> name;
    ASSERT_TRUE(name.assign("Nguyễn").has_value());

    std::array<std::byte, 128> buffer{};
    Encoder encoder(buffer);
    encoder.write_bool(true);
    encoder.write_bits(0x2A, 6);
    encoder.write_int(i16{-300}, -1'000, 1'000);
    encoder.write_int(u64{7}, 0, 7);
    encoder.write_quantized(1.25, yaw);
    encoder.write_bytes(key);
    encoder.write_string(name);
    encoder.write_length(3, 64);
    encoder.write_tick(Tick{0xFEDC'BA98'7654'3210});
    encoder.write_short_tick(ShortTick{0xBEEF});
    encoder.write_replicated_id(ReplicatedId{123'456});
    const Result<usize> size = encoder.finish();
    ASSERT_TRUE(size.has_value());

    Decoder decoder(std::span(buffer).first(*size));
    bool flag = false;
    u8 bits = 0;
    i16 signed_value = 0;
    u64 small = 0;
    f64 angle = 0;
    BoundedBytes<16> key_read;
    BoundedString<32> name_read;
    Tick tick;
    ShortTick short_tick;
    ReplicatedId id;
    decoder.read_bool(flag);
    decoder.read_bits(bits, 6);
    decoder.read_int(signed_value, -1'000, 1'000);
    decoder.read_int(small, 0, 7);
    decoder.read_quantized(angle, yaw);
    decoder.read_bytes(key_read);
    decoder.read_string(name_read);
    const usize length = decoder.read_length(64);
    decoder.read_tick(tick);
    decoder.read_short_tick(short_tick);
    decoder.read_replicated_id(id);
    ASSERT_TRUE(decoder.finish().has_value());
    EXPECT_TRUE(flag);
    EXPECT_EQ(bits, 0x2AU);
    EXPECT_EQ(signed_value, -300);
    EXPECT_EQ(small, 7U);
    EXPECT_NEAR(angle, 1.25, 0.005);
    EXPECT_EQ(key_read, key);
    EXPECT_EQ(name_read, name);
    EXPECT_EQ(length, 3U);
    EXPECT_EQ(tick, Tick{0xFEDC'BA98'7654'3210});
    EXPECT_EQ(short_tick, ShortTick{0xBEEF});
    EXPECT_EQ(id, ReplicatedId{123'456});
}

TEST(Encoder, ValuesOutsideTheSchemaAreInvalidArgument) {
    std::array<std::byte, 32> buffer{};
    const auto finish_code = [](const Encoder& encoder) {
        const Result<usize> result = encoder.finish();
        return result.has_value() ? ErrorCode::Internal : result.error().code();
    };
    Encoder above(buffer);
    above.write_int(i32{11}, 0, 10);
    EXPECT_EQ(finish_code(above), ErrorCode::InvalidArgument);
    Encoder below(buffer);
    below.write_int(i32{-1}, 0, 10);
    EXPECT_EQ(finish_code(below), ErrorCode::InvalidArgument);
    Encoder huge(buffer);
    huge.write_int(std::numeric_limits<u64>::max(), 0, std::numeric_limits<i64>::max());
    EXPECT_EQ(finish_code(huge), ErrorCode::InvalidArgument);
    Encoder wide(buffer);
    wide.write_bits(8, 3);
    EXPECT_EQ(finish_code(wide), ErrorCode::InvalidArgument);
    Encoder rejected(buffer);
    rejected.reject();
    EXPECT_EQ(finish_code(rejected), ErrorCode::InvalidArgument);
    // Sau lỗi đầu tiên không ghi gì thêm, kể cả trường đúng.
    Encoder sticky(buffer);
    sticky.write_int(i32{11}, 0, 10);
    sticky.write_bool(true);
    EXPECT_FALSE(sticky.ok());
    EXPECT_EQ(finish_code(sticky), ErrorCode::InvalidArgument);
}

TEST(Encoder, TooSmallABufferIsResourceExhausted) {
    std::array<std::byte, 1> buffer{};
    Encoder encoder(buffer);
    encoder.write_bits(0xFFFF, 16);
    const Result<usize> result = encoder.finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::ResourceExhausted);
}

TEST(Decoder, FirstErrorWinsAndLaterReadsDoNothing) {
    const std::array data = {std::byte{0xFF}};
    Decoder decoder(data);
    u16 wide = 0;
    decoder.read_bits(wide, 16);  // Chỉ còn 8 bit: OutOfRange.
    bool flag = false;
    decoder.read_bool(flag);
    decoder.reject();
    EXPECT_FALSE(flag);
    EXPECT_EQ(decoder.read_length(8), 0U);
    f64 value = 1.0;
    decoder.read_quantized(value, net::Quantization(0.0, 1.0, 0.5));
    BoundedBytes<2> bytes;
    decoder.read_bytes(bytes);
    BoundedString<2> text;
    decoder.read_string(text);
    Tick tick{9};
    decoder.read_tick(tick);
    EXPECT_EQ(value, 1.0);
    EXPECT_EQ(bytes.size(), 0U);
    EXPECT_EQ(text.size(), 0U);
    EXPECT_EQ(tick, Tick{9});
    const Result<void> result = decoder.finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(Decoder, EveryReadOfAnEmptyInputIsOutOfRange) {
    Decoder bool_decoder({});
    bool flag = false;
    bool_decoder.read_bool(flag);
    EXPECT_EQ(bool_decoder.finish().error().code(), ErrorCode::OutOfRange);
    Decoder quantized_decoder({});
    f64 value = 0.0;
    quantized_decoder.read_quantized(value, net::Quantization(0.0, 1.0, 0.5));
    EXPECT_EQ(quantized_decoder.finish().error().code(), ErrorCode::OutOfRange);
    Decoder string_decoder({});
    BoundedString<2> text;
    string_decoder.read_string(text);
    EXPECT_EQ(string_decoder.finish().error().code(), ErrorCode::OutOfRange);
}

TEST(Decoder, RejectAndTrailingBitsAreInvalidArgument) {
    const std::array data = {std::byte{0x01}};
    Decoder rejected(data);
    bool flag = false;
    rejected.read_bool(flag);
    rejected.reject();
    EXPECT_EQ(rejected.finish().error().code(), ErrorCode::InvalidArgument);
    const std::array padded = {std::byte{0x03}};
    Decoder trailing(padded);
    trailing.read_bool(flag);
    EXPECT_EQ(trailing.finish().error().code(), ErrorCode::InvalidArgument);
    // Giá trị vượt khoảng, độ dài vượt max, chuỗi không phải UTF-8.
    const std::array over = {std::byte{0x0F}};
    Decoder ranged(over);
    u8 value = 0;
    ranged.read_int(value, 0, 10);
    EXPECT_EQ(ranged.finish().error().code(), ErrorCode::InvalidArgument);
    std::array<std::byte, 8> buffer{};
    net::BitWriter writer(buffer);
    writer.write_bits(1, 2);
    writer.write_bits(0xC3, 8);
    Decoder broken(writer.written());
    BoundedString<3> text;
    broken.read_string(text);
    EXPECT_EQ(broken.finish().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(text.size(), 0U);
    Decoder truncated(std::span(buffer).first(0));
    BoundedBytes<3> bytes;
    truncated.read_bytes(bytes);
    EXPECT_EQ(truncated.finish().error().code(), ErrorCode::OutOfRange);
}

// X.7: ghi và đọc không cấp phát.
TEST(Codec, DoesNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    BoundedString<32> name;
    ASSERT_TRUE(name.assign("Orion").has_value());
    std::array<std::byte, 64> buffer{};
    const core::testing::AllocationScope scope;
    for (u32 i = 0; i < 100; ++i) {
        Encoder encoder(buffer);
        encoder.write_string(name);
        encoder.write_int(i, 0, 1'000);
        const usize size = encoder.finish().value_or(0);
        Decoder decoder(std::span(buffer).first(size));
        BoundedString<32> read_name;
        u32 value = 0;
        decoder.read_string(read_name);
        decoder.read_int(value, 0, 1'000);
        ASSERT_TRUE(decoder.finish().has_value());
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::protocol
