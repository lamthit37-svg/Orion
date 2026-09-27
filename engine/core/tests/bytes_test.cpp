#include "engine/core/bytes.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

namespace orion::core {
namespace {

constexpr std::array<std::byte, 4> kBytes{std::byte{0x04}, std::byte{0x03}, std::byte{0x02},
                                          std::byte{0x01}};
static_assert(load_le<u32>(std::span<const std::byte, 4>(kBytes)) == 0x01020304U);
static_assert(load_le<i16>(std::span<const std::byte, 2>(kBytes.data(), 2)) == 0x0304);

template <std::integral T>
void expect_round_trip(const T value) {
    std::array<std::byte, sizeof(T)> buffer{};
    store_le<T>(buffer, value);
    EXPECT_EQ(load_le<T>(buffer), value);
}

TEST(Bytes, LittleEndianLayoutIsExplicit) {
    std::array<std::byte, 8> buffer{};
    store_le<u64>(buffer, 0x0102030405060708ULL);
    const std::array<std::byte, 8> expected{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
                                            std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
    EXPECT_EQ(buffer, expected);
}

TEST(Bytes, IntegersRoundTripAtTheirLimits) {
    expect_round_trip<u8>(0xFF);
    expect_round_trip<i8>(-128);
    expect_round_trip<u16>(0xBEEF);
    expect_round_trip<i16>(std::numeric_limits<i16>::min());
    expect_round_trip<u32>(0xDEADBEEFU);
    expect_round_trip<i32>(-1);
    expect_round_trip<u64>(std::numeric_limits<u64>::max());
    expect_round_trip<i64>(std::numeric_limits<i64>::min());
}

TEST(ByteReader, ReadsSequentiallyAndStopsAtTheEnd) {
    const std::array<std::byte, 7> data{std::byte{0x2A}, std::byte{0x34}, std::byte{0x12},
                                        std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x00}};
    ByteReader reader{data};
    EXPECT_EQ(reader.read<u8>(), Result<u8>{0x2A});
    EXPECT_EQ(reader.read<u16>(), Result<u16>{0x1234});
    EXPECT_EQ(reader.offset(), 3U);
    const auto truncated = reader.read<u64>();
    ASSERT_FALSE(truncated.has_value());
    EXPECT_EQ(truncated.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(truncated.error().detail(), 3) << "lỗi ghi vị trí hỏng";
    EXPECT_EQ(reader.offset(), 3U) << "đọc hỏng không được đổi vị trí";
    EXPECT_EQ(reader.read<u32>(), Result<u32>{1});
    EXPECT_TRUE(reader.at_end());
    EXPECT_FALSE(reader.read_bytes(1).has_value());
}

TEST(ByteReader, ReadBytesPointsIntoTheSource) {
    const std::array<std::byte, 3> data{std::byte{1}, std::byte{2}, std::byte{3}};
    ByteReader reader{data};
    const auto bytes = reader.read_bytes(2);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->data(), data.data());
    EXPECT_EQ(bytes->size(), 2U);
    EXPECT_EQ(reader.remaining(), 1U);
}

TEST(ByteWriterReader, FloatsKeepEveryBit) {
    std::array<std::byte, 12> buffer{};
    ByteWriter writer{buffer};
    const f32 nan_with_payload = std::bit_cast<f32>(0x7FC0'1234U);
    writer.write_f32(nan_with_payload);
    writer.write_f64(-0.0);
    ASSERT_FALSE(writer.overflowed());
    ByteReader reader{writer.written()};
    const auto first = reader.read_f32();
    const auto second = reader.read_f64();
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(std::bit_cast<u32>(*first), 0x7FC0'1234U);
    EXPECT_EQ(std::bit_cast<u64>(*second), std::bit_cast<u64>(-0.0));
}

TEST(Bytes, UnsignedCharViewsAliasTheSameMemory) {
    std::array<std::byte, 3> buffer{std::byte{0x10}, std::byte{0x20}, std::byte{0xFF}};
    const std::span<const unsigned char> read = as_uchars(buffer);
    ASSERT_EQ(read.size(), 3U);
    EXPECT_EQ(read[0], 0x10U);
    EXPECT_EQ(read[2], 0xFFU);
    const std::span<unsigned char> write = as_writable_uchars(buffer);
    write[1] = 0x7F;
    EXPECT_EQ(buffer[1], std::byte{0x7F});
    EXPECT_EQ(static_cast<const void*>(read.data()), static_cast<const void*>(buffer.data()));
    EXPECT_TRUE(as_uchars({}).empty());
}

TEST(Bytes, CharViewSharesMemoryWithoutCopying) {
    const std::array<std::byte, 4> buffer{std::byte{'p'}, std::byte{'a'}, std::byte{'k'},
                                          std::byte{0xC3}};
    const std::string_view text = as_chars(buffer);
    ASSERT_EQ(text.size(), 4U);
    EXPECT_EQ(text.substr(0, 3), "pak");
    EXPECT_EQ(static_cast<unsigned char>(text[3]), 0xC3U);
    EXPECT_EQ(static_cast<const void*>(text.data()), static_cast<const void*>(buffer.data()));
    EXPECT_TRUE(as_chars({}).empty());
}

TEST(ByteWriter, OverflowWritesNothingFurther) {
    std::array<std::byte, 5> buffer{};
    ByteWriter writer{buffer};
    writer.write<u32>(0x11223344U);
    writer.write<u16>(0x5566);
    EXPECT_TRUE(writer.overflowed());
    EXPECT_EQ(writer.offset(), 4U);
    writer.write<u8>(0x77);
    EXPECT_EQ(writer.offset(), 4U) << "đã tràn thì mọi lần ghi sau bị bỏ";
    EXPECT_EQ(buffer[4], std::byte{0});
}

}  // namespace
}  // namespace orion::core
