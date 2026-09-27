#include "engine/net/bitstream.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/math/random.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orion::net {
namespace {

[[nodiscard]] std::string hex(const std::span<const std::byte> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    for (const std::byte b : bytes) {
        const auto value = std::to_integer<u32>(b);
        text.push_back(kDigits[value >> 4U]);
        text.push_back(kDigits[value & 0x0FU]);
    }
    return text;
}

const Quantization kCentimetres(-10.0, 10.0, 0.01);

TEST(BitWriter, LowBitsGoFirst) {
    std::array<std::byte, 4> buffer{};
    BitWriter writer(buffer);
    writer.write_bits(1, 1);
    writer.write_bits(0b10, 2);
    writer.write_bits(0x7F, 7);
    ASSERT_FALSE(writer.overflowed());
    EXPECT_EQ(writer.bit_count(), 10U);
    EXPECT_EQ(hex(writer.written()), "fd03");
}

// Bộ byte mong đợi do một bộ ghi Python viết độc lập theo docs/formats/protocol.md sinh ra (commit
// thêm tệp này ghi lại đoạn mã đó). Test chạy ở mọi toolchain nên giữ định dạng trên dây giống nhau
// giữa MSVC, clang-cl, clang và NDK.
TEST(BitWriter, GoldenBytesFollowTheSpecification) {
    std::array<std::byte, 64> buffer{};
    BitWriter writer(buffer);
    writer.write_bool(true);
    writer.write_bits(5, 3);
    writer.write_ranged(-1000, -1000, 1000);
    writer.write_ranged(1000, -1000, 1000);
    writer.write_ranged(-7, std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max());
    writer.write_quantized(4.56789, kCentimetres);
    writer.write_quantized(-1.23456, kCentimetres);
    const std::array bytes = {std::byte{0xDE}, std::byte{0xAD}};
    writer.write_bytes(bytes, 16);
    writer.write_string("Việt", 32);
    writer.write_bits(0x0123'4567'89AB'CDEFULL, 64);
    ASSERT_FALSE(writer.overflowed());
    EXPECT_EQ(writer.bit_count(), 251U);
    EXPECT_EQ(hex(writer.written()),
              "0b00e8e7ffffffffffffffc5b66dc2bbd5b04a0bdf3da47b6f5e4d3c2b1a0900");

    BitReader reader(writer.written());
    EXPECT_EQ(reader.read_bool().value_or(false), true);
    EXPECT_EQ(reader.read_bits(3).value_or(0), 5U);
    EXPECT_EQ(reader.read_ranged(-1000, 1000).value_or(0), -1000);
    EXPECT_EQ(reader.read_ranged(-1000, 1000).value_or(0), 1000);
    EXPECT_EQ(reader.read_ranged(std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max())
                  .value_or(0),
              -7);
    EXPECT_EQ(reader.read_quantized(kCentimetres).value_or(0.0), kCentimetres.value_of(1457));
    EXPECT_EQ(reader.read_quantized(kCentimetres).value_or(0.0), kCentimetres.value_of(877));
    std::array<std::byte, 16> read_bytes{};
    EXPECT_EQ(reader.read_bytes(read_bytes, 16).value_or(0), 2U);
    EXPECT_EQ(read_bytes[0], std::byte{0xDE});
    EXPECT_EQ(read_bytes[1], std::byte{0xAD});
    std::array<char, 32> text{};
    const Result<usize> text_size = reader.read_string(text, 32);
    ASSERT_TRUE(text_size.has_value());
    EXPECT_EQ(std::string_view(text.data(), *text_size), "Việt");
    EXPECT_EQ(reader.read_bits(64).value_or(0), 0x0123'4567'89AB'CDEFULL);
    EXPECT_TRUE(reader.finish().has_value());
}

TEST(BitWriter, ZeroBitsAndSingleValueRangesWriteNothing) {
    std::array<std::byte, 1> buffer{};
    BitWriter writer(buffer);
    writer.write_bits(0, 0);
    writer.write_ranged(5, 5, 5);
    EXPECT_EQ(writer.bit_count(), 0U);
    EXPECT_TRUE(writer.written().empty());
    BitReader reader(writer.written());
    EXPECT_EQ(reader.read_ranged(5, 5).value_or(0), 5);
    EXPECT_TRUE(reader.finish().has_value());
}

TEST(BitWriter, OverflowStopsWritingAndIsReported) {
    std::array<std::byte, 1> buffer{};
    BitWriter writer(buffer);
    writer.write_bits(0xAB, 8);
    EXPECT_FALSE(writer.overflowed());
    writer.write_bits(1, 1);
    EXPECT_TRUE(writer.overflowed());
    writer.write_bool(false);
    EXPECT_EQ(writer.bit_count(), 8U);
    EXPECT_EQ(buffer[0], std::byte{0xAB});
}

TEST(Quantization, IndexAndValueFollowTheSpecification) {
    EXPECT_EQ(kCentimetres.min(), -10.0);
    EXPECT_EQ(kCentimetres.max(), 10.0);
    EXPECT_EQ(kCentimetres.step(), 0.01);
    EXPECT_EQ(kCentimetres.max_index(), 2000U);
    EXPECT_EQ(kCentimetres.bits(), 11U);
    EXPECT_EQ(kCentimetres.index_of(0.0), 1000U);
    EXPECT_EQ(kCentimetres.value_of(1000), 0.0);
    EXPECT_EQ(kCentimetres.index_of(-10.0), 0U);
    EXPECT_EQ(kCentimetres.index_of(10.0), 2000U);
    EXPECT_EQ(kCentimetres.value_of(2000), 10.0);
    // Ngoài khoảng thì kẹp; NaN cho min.
    EXPECT_EQ(kCentimetres.index_of(123.0), 2000U);
    EXPECT_EQ(kCentimetres.index_of(-std::numeric_limits<f64>::infinity()), 0U);
    EXPECT_EQ(kCentimetres.index_of(std::numeric_limits<f64>::quiet_NaN()), 0U);
    // Bước không chia hết khoảng: chỉ số cuối vẫn cho đúng max.
    const Quantization uneven(0.0, 1.0, 0.3);
    EXPECT_EQ(uneven.max_index(), 4U);
    EXPECT_EQ(uneven.value_of(4), 1.0);
    EXPECT_EQ(uneven.index_of(1.0), 4U);
}

TEST(Quantization, RoundTripErrorIsAtMostHalfAStep) {
    math::Pcg32 random(2026, 9);
    for (u32 i = 0; i < 10'000; ++i) {
        const f64 value = -10.0 + (20.0 * static_cast<f64>(random.next_u32()) / 4294967296.0);
        const f64 decoded = kCentimetres.value_of(kCentimetres.index_of(value));
        EXPECT_LE(decoded - value, 0.005 + 1e-12) << value;
        EXPECT_LE(value - decoded, 0.005 + 1e-12) << value;
    }
}

TEST(BitReader, ValuesOutsideTheDeclaredRangeAreRejected) {
    std::array<std::byte, 8> buffer{};
    BitWriter writer(buffer);
    writer.write_bits(7, 3);  // [0, 5] cần 3 bit, 7 vượt khoảng.
    writer.write_bits(7, 3);  // Quantization(0, 1, 0.3): max_index 4 trên 3 bit.
    writer.write_bits(7, 3);  // Độ dài 7 với max_size 5.
    BitReader reader(writer.written());
    const Result<i64> ranged = reader.read_ranged(0, 5);
    ASSERT_FALSE(ranged.has_value());
    EXPECT_EQ(ranged.error().code(), ErrorCode::InvalidArgument);
    // Vị trí đọc không đổi khi lỗi.
    EXPECT_EQ(reader.read_bits(3).value_or(0), 7U);
    const Result<f64> quantized = reader.read_quantized(Quantization(0.0, 1.0, 0.3));
    ASSERT_FALSE(quantized.has_value());
    EXPECT_EQ(quantized.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(reader.read_bits(3).value_or(0), 7U);
    std::array<std::byte, 5> out{};
    const Result<usize> bytes = reader.read_bytes(out, 5);
    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().code(), ErrorCode::InvalidArgument);
}

TEST(BitReader, TruncatedInputIsOutOfRange) {
    const std::array data = {std::byte{0xFF}};
    BitReader reader(data);
    const Result<u64> wide = reader.read_bits(9);
    ASSERT_FALSE(wide.has_value());
    EXPECT_EQ(wide.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(reader.bits_remaining(), 8U);
    // Độ dài 3 (trên 2 bit của max_size 3) nhưng chỉ còn 6 bit.
    std::array<std::byte, 3> out{};
    const Result<usize> bytes = reader.read_bytes(out, 3);
    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(reader.bits_remaining(), 8U);
    const Result<bool> empty = BitReader({}).read_bool();
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code(), ErrorCode::OutOfRange);
}

// write_bytes và read_bytes chép cả khối; kết quả phải trùng từng bit với cách ghi từng byte bằng
// write_bits(b, 8), ở mọi độ lệch 0..7, kể cả khi bộ đệm ghi chứa rác từ trước.
TEST(BitStream, BytesMatchByteByByteEncodingAtEveryOffset) {
    constexpr usize kMax = 300;
    for (u32 offset = 0; offset < 8; ++offset) {
        for (const usize size : {usize{0}, usize{1}, usize{2}, usize{7}, kMax}) {
            std::vector<std::byte> data(size);
            for (usize i = 0; i < size; ++i) {
                data[i] = static_cast<std::byte>(((i * 37U) + offset) & 0xFFU);
            }
            std::array<std::byte, 320> block_buffer{};
            std::array<std::byte, 320> reference_buffer{};
            block_buffer.fill(std::byte{0xA5});
            reference_buffer.fill(std::byte{0x5A});
            BitWriter block(block_buffer);
            BitWriter reference(reference_buffer);
            const u64 prefix = 0x5BU & ((u64{1} << offset) - 1);
            block.write_bits(prefix, offset);
            reference.write_bits(prefix, offset);
            block.write_bytes(data, kMax);
            reference.write_bits(size, bits_for(kMax));
            for (const std::byte b : data) {
                reference.write_bits(std::to_integer<u64>(b), 8);
            }
            block.write_bool(true);
            reference.write_bool(true);
            ASSERT_FALSE(block.overflowed() || reference.overflowed());
            ASSERT_EQ(hex(block.written()), hex(reference.written()))
                << "lệch " << offset << " bit, " << size << " byte";

            BitReader reader(block.written());
            EXPECT_EQ(reader.read_bits(offset).value_or(~u64{0}), prefix);
            std::array<std::byte, kMax> out{};
            const Result<usize> read = reader.read_bytes(out, kMax);
            ASSERT_EQ(read.value_or(kMax + 1), size);
            EXPECT_TRUE(std::ranges::equal(std::span(out).first(size), data));
            EXPECT_TRUE(reader.read_bool().value_or(false));
            EXPECT_TRUE(reader.finish().has_value());
        }
    }
}

// Thiếu một byte ở bất kỳ độ lệch nào: bên ghi báo tràn, bên đọc trả OutOfRange mà không dời vị
// trí.
TEST(BitStream, BytesOneByteShortFailAtEveryOffset) {
    const std::array data = {std::byte{1}, std::byte{2}, std::byte{3}};
    for (u32 offset = 0; offset < 8; ++offset) {
        // Độ dài 2 bit (max_size 3) rồi 24 bit dữ liệu.
        const usize bits = offset + 2 + 24;
        std::vector<std::byte> exact((bits + 7) / 8);
        BitWriter fits(exact);
        fits.write_bits(0, offset);
        fits.write_bytes(data, 3);
        ASSERT_FALSE(fits.overflowed()) << offset;
        EXPECT_EQ(fits.bit_count(), bits);

        std::vector<std::byte> short_buffer(exact.size() - 1);
        BitWriter tight(short_buffer);
        tight.write_bits(0, offset);
        tight.write_bytes(data, 3);
        EXPECT_TRUE(tight.overflowed()) << offset;

        BitReader reader(std::span<const std::byte>(exact).first(exact.size() - 1));
        ASSERT_TRUE(reader.read_bits(offset).has_value());
        const usize before = reader.bits_remaining();
        std::array<std::byte, 3> out{};
        const Result<usize> read = reader.read_bytes(out, 3);
        ASSERT_FALSE(read.has_value()) << offset;
        EXPECT_EQ(read.error().code(), ErrorCode::OutOfRange);
        EXPECT_EQ(reader.bits_remaining(), before);
    }
}

TEST(BitReader, StringsMustBeUtf8) {
    std::array<std::byte, 8> buffer{};
    BitWriter writer(buffer);
    const std::array broken = {std::byte{0xC3}};
    writer.write_bytes(broken, 4);
    BitReader reader(writer.written());
    std::array<char, 4> out{};
    const Result<usize> text = reader.read_string(out, 4);
    ASSERT_FALSE(text.has_value());
    EXPECT_EQ(text.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(reader.bits_remaining(), writer.written().size() * 8);
}

TEST(BitReader, FinishRequiresZeroPaddingAndNoTrailingBytes) {
    const std::array exact = {std::byte{0x05}};
    BitReader reader(exact);
    ASSERT_EQ(reader.read_bits(3).value_or(0), 5U);
    EXPECT_TRUE(reader.finish().has_value());

    const std::array dirty = {std::byte{0x85}};
    BitReader dirty_reader(dirty);
    ASSERT_EQ(dirty_reader.read_bits(3).value_or(0), 5U);
    const Result<void> padding = dirty_reader.finish();
    ASSERT_FALSE(padding.has_value());
    EXPECT_EQ(padding.error().code(), ErrorCode::InvalidArgument);

    const std::array trailing = {std::byte{0x05}, std::byte{0x00}};
    BitReader trailing_reader(trailing);
    ASSERT_EQ(trailing_reader.read_bits(3).value_or(0), 5U);
    const Result<void> extra = trailing_reader.finish();
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code(), ErrorCode::InvalidArgument);

    EXPECT_TRUE(BitReader({}).finish().has_value());
}

// Dãy trường ngẫu nhiên: đọc lại đúng giá trị, và ghi lại những gì đọc được thì ra đúng dãy byte cũ
// (mỗi tin nhắn có một cách mã hoá).
TEST(BitStream, RandomFieldsRoundTripCanonically) {
    math::Pcg32 random(7, 11);
    for (u32 round = 0; round < 500; ++round) {
        struct Field {
            u32 kind;
            u64 value;
            u32 bits;
        };
        std::vector<Field> fields;
        std::array<std::byte, 512> buffer{};
        BitWriter writer(buffer);
        const u32 count = random.next_below(40);
        for (u32 i = 0; i < count; ++i) {
            Field field{random.next_below(3), 0, 0};
            if (field.kind == 0) {
                field.bits = random.next_below(65);
                field.value = field.bits == 0 ? 0 : random.next_u64() >> (64U - field.bits);
                writer.write_bits(field.value, field.bits);
            } else if (field.kind == 1) {
                field.value = random.next_below(2);
                writer.write_bool(field.value != 0);
            } else {
                field.value = random.next_below(2001);
                writer.write_ranged(static_cast<i64>(field.value) - 1000, -1000, 1000);
            }
            fields.push_back(field);
        }
        ASSERT_FALSE(writer.overflowed());

        BitReader reader(writer.written());
        std::array<std::byte, 512> again{};
        BitWriter rewriter(again);
        for (const Field& field : fields) {
            if (field.kind == 0) {
                const Result<u64> value = reader.read_bits(field.bits);
                ASSERT_TRUE(value.has_value());
                EXPECT_EQ(*value, field.value);
                rewriter.write_bits(*value, field.bits);
            } else if (field.kind == 1) {
                const Result<bool> value = reader.read_bool();
                ASSERT_TRUE(value.has_value());
                EXPECT_EQ(*value, field.value != 0);
                rewriter.write_bool(*value);
            } else {
                const Result<i64> value = reader.read_ranged(-1000, 1000);
                ASSERT_TRUE(value.has_value());
                EXPECT_EQ(*value, static_cast<i64>(field.value) - 1000);
                rewriter.write_ranged(*value, -1000, 1000);
            }
        }
        EXPECT_TRUE(reader.finish().has_value());
        EXPECT_EQ(hex(rewriter.written()), hex(writer.written()));
    }
}

}  // namespace
}  // namespace orion::net
