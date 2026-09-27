// Giá trị nhị phân của PostgreSQL (value.hpp). Byte mong đợi viết tay theo hàm send, recv của từng
// kiểu trong mã nguồn PostgreSQL (int8send, float8send, boolsend, timestamptz_send), không suy ra
// từ chính code được test.

#include "game/server/lib/db/value.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace orion::db {
namespace {

template <class... Bytes>
[[nodiscard]] std::vector<std::byte> bytes_of(const Bytes... values) {
    return {static_cast<std::byte>(values)...};
}

[[nodiscard]] std::vector<std::byte> wire_of(const Param& param) {
    const std::span<const std::byte> wire = param.wire();
    return {wire.begin(), wire.end()};
}

constexpr i64 kMin = std::numeric_limits<i64>::min();
constexpr i64 kMax = std::numeric_limits<i64>::max();

TEST(Param, NumbersAreBigEndian) {
    const Param integer = Param::integer(0x0102'0304'0506'0708);
    EXPECT_EQ(integer.type(), oid::kInt8);
    EXPECT_FALSE(integer.is_null());
    EXPECT_EQ(wire_of(integer), bytes_of(1, 2, 3, 4, 5, 6, 7, 8));
    EXPECT_EQ(wire_of(Param::integer(-2)),
              bytes_of(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE));
    EXPECT_EQ(wire_of(Param::integer(kMin)), bytes_of(0x80, 0, 0, 0, 0, 0, 0, 0));

    // 1,5 = 0x3FF8000000000000; -0 giữ bit dấu.
    const Param real = Param::real(1.5);
    EXPECT_EQ(real.type(), oid::kFloat8);
    EXPECT_EQ(wire_of(real), bytes_of(0x3F, 0xF8, 0, 0, 0, 0, 0, 0));
    EXPECT_EQ(wire_of(Param::real(-0.0)), bytes_of(0x80, 0, 0, 0, 0, 0, 0, 0));

    EXPECT_EQ(Param::boolean(true).type(), oid::kBool);
    EXPECT_EQ(wire_of(Param::boolean(true)), bytes_of(1));
    EXPECT_EQ(wire_of(Param::boolean(false)), bytes_of(0));
}

TEST(Param, TextAndBytesPointAtTheCallersMemory) {
    const std::string_view text = "Hà Nội";
    const Param param = Param::text(text);
    EXPECT_EQ(param.type(), oid::kText);
    EXPECT_EQ(param.wire().data(), static_cast<const void*>(text.data()));
    EXPECT_EQ(param.wire().size(), text.size());

    const std::array<std::byte, 3> raw{std::byte{0}, std::byte{0xFF}, std::byte{7}};
    const Param bytes = Param::bytes(raw);
    EXPECT_EQ(bytes.type(), oid::kBytea);
    EXPECT_EQ(bytes.wire().data(), raw.data());
    EXPECT_EQ(bytes.wire().size(), raw.size());

    // Chuỗi rỗng không phải NULL.
    EXPECT_FALSE(Param::text("").is_null());
    EXPECT_TRUE(Param::text("").wire().empty());
}

TEST(Param, NullHasNoTypeAndNoBytes) {
    const Param null = Param::null();
    EXPECT_TRUE(null.is_null());
    EXPECT_EQ(null.type(), oid::kUnknown);
    EXPECT_TRUE(null.wire().empty());
}

TEST(Param, TimeCountsMicrosecondsFrom2000) {
    const Param epoch =
        Param::time(core::WallTime::from_unix_microseconds(kPostgresEpochUnixMicros));
    EXPECT_EQ(epoch.type(), oid::kTimestampTz);
    EXPECT_EQ(wire_of(epoch), bytes_of(0, 0, 0, 0, 0, 0, 0, 0));
    // Unix epoch là 946 684 800 giây trước 2000-01-01: -946684800000000 = 0xFFFCA2FE_C4C82000.
    EXPECT_EQ(wire_of(Param::time(core::WallTime{})),
              bytes_of(0xFF, 0xFC, 0xA2, 0xFE, 0xC4, 0xC8, 0x20, 0x00));
}

TEST(Time, ConvertsBothWaysAndGuardsTheEdges) {
    EXPECT_EQ(to_postgres_micros(core::WallTime{}), -kPostgresEpochUnixMicros);
    EXPECT_EQ(to_postgres_micros(core::WallTime::from_unix_seconds(1'700'000'000)),
              1'700'000'000'000'000 - kPostgresEpochUnixMicros);
    // Phép trừ tràn, hay ra đúng -infinity: thành giá trị hữu hạn server từ chối.
    EXPECT_EQ(to_postgres_micros(core::WallTime::from_unix_microseconds(kMin)), kMin + 1);
    EXPECT_EQ(
        to_postgres_micros(core::WallTime::from_unix_microseconds(kMin + kPostgresEpochUnixMicros)),
        kMin + 1);
    EXPECT_EQ(to_postgres_micros(
                  core::WallTime::from_unix_microseconds(kMin + kPostgresEpochUnixMicros + 1)),
              kMin + 1);
    EXPECT_EQ(to_postgres_micros(core::WallTime::from_unix_microseconds(kMax)),
              kMax - kPostgresEpochUnixMicros);

    const Result<core::WallTime> zero = from_postgres_micros(0);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(zero->unix_microseconds(), kPostgresEpochUnixMicros);
    const Result<core::WallTime> last = from_postgres_micros(kMax - kPostgresEpochUnixMicros);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->unix_microseconds(), kMax);
    EXPECT_EQ(from_postgres_micros(kMax - kPostgresEpochUnixMicros + 1).error().code(),
              ErrorCode::OutOfRange);
    EXPECT_EQ(from_postgres_micros(kMax).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(from_postgres_micros(kMin).error().code(), ErrorCode::OutOfRange);
    const Result<core::WallTime> early = from_postgres_micros(kMin + 1);
    ASSERT_TRUE(early.has_value());
    EXPECT_EQ(early->unix_microseconds(), kMin + 1 + kPostgresEpochUnixMicros);
}

TEST(Decode, IntegersOfEveryWidth) {
    const Result<i64> small = decode_integer(oid::kInt2, bytes_of(0xFF, 0xFE));
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(*small, -2);
    const Result<i64> medium = decode_integer(oid::kInt4, bytes_of(0x7F, 0xFF, 0xFF, 0xFF));
    ASSERT_TRUE(medium.has_value());
    EXPECT_EQ(*medium, std::numeric_limits<i32>::max());
    const Result<i64> large = decode_integer(oid::kInt8, bytes_of(0x80, 0, 0, 0, 0, 0, 0, 0));
    ASSERT_TRUE(large.has_value());
    EXPECT_EQ(*large, kMin);
}

TEST(Decode, RealsKeepTheirValue) {
    const Result<f64> single = decode_real(oid::kFloat4, bytes_of(0x3F, 0xC0, 0, 0));
    ASSERT_TRUE(single.has_value());
    EXPECT_EQ(*single, 1.5);
    const Result<f64> nan = decode_real(oid::kFloat8, bytes_of(0x7F, 0xF8, 0, 0, 0, 0, 0, 1));
    ASSERT_TRUE(nan.has_value());
    EXPECT_EQ(std::bit_cast<u64>(*nan), 0x7FF8'0000'0000'0001U);
    const Result<f64> infinity = decode_real(oid::kFloat4, bytes_of(0xFF, 0x80, 0, 0));
    ASSERT_TRUE(infinity.has_value());
    EXPECT_EQ(*infinity, -std::numeric_limits<f64>::infinity());
    // NaN báo hiệu của float4 thành NaN yên khi đổi sang f64 (IEEE 754), nhưng vẫn là NaN.
    const Result<f64> signaling = decode_real(oid::kFloat4, bytes_of(0x7F, 0x80, 0, 1));
    ASSERT_TRUE(signaling.has_value());
    const auto wide = std::bit_cast<u64>(*signaling);
    EXPECT_EQ(wide & 0x7FF0'0000'0000'0000U, 0x7FF0'0000'0000'0000U);
    EXPECT_NE(wide & 0x000F'FFFF'FFFF'FFFFU, 0U);
}

TEST(Decode, BooleansTextBytesAndTime) {
    EXPECT_EQ(decode_boolean(oid::kBool, bytes_of(1)), true);
    EXPECT_EQ(decode_boolean(oid::kBool, bytes_of(0)), false);
    const std::vector<std::byte> hello = bytes_of('x', 0, 'y');
    for (const u32 type : {oid::kText, oid::kVarchar, oid::kName}) {
        const Result<std::string_view> text = decode_text(type, hello);
        ASSERT_TRUE(text.has_value()) << type;
        EXPECT_EQ(*text, std::string_view("x\0y", 3));
    }
    const Result<std::span<const std::byte>> raw = decode_bytes(oid::kBytea, hello);
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw->data(), hello.data());
    EXPECT_EQ(raw->size(), 3U);
    const Result<core::WallTime> time =
        decode_time(oid::kTimestampTz, bytes_of(0, 0, 0, 0, 0, 0, 0, 1));
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(time->unix_microseconds(), kPostgresEpochUnixMicros + 1);
}

TEST(Decode, WrongTypeIsInvalidArgumentWithTheColumnOid) {
    const std::vector<std::byte> eight = bytes_of(0, 0, 0, 0, 0, 0, 0, 0);
    const Result<i64> integer = decode_integer(oid::kText, eight);
    ASSERT_FALSE(integer.has_value());
    EXPECT_EQ(integer.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(integer.error().detail(), oid::kText);
    EXPECT_EQ(decode_boolean(oid::kInt2, bytes_of(0)).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_real(oid::kInt8, eight).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_text(oid::kBytea, eight).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(decode_bytes(oid::kText, eight).error().code(), ErrorCode::InvalidArgument);
    // timestamp không có múi giờ là kiểu khác (1114), dù cùng định dạng nhị phân.
    EXPECT_EQ(decode_time(1'114, eight).error().code(), ErrorCode::InvalidArgument);
}

TEST(Decode, WrongLengthOrContentIsDataLoss) {
    const Result<i64> short_int = decode_integer(oid::kInt4, bytes_of(0, 0, 0));
    ASSERT_FALSE(short_int.has_value());
    EXPECT_EQ(short_int.error().code(), ErrorCode::DataLoss);
    EXPECT_EQ(short_int.error().detail(), 3);
    EXPECT_EQ(decode_integer(oid::kInt2, bytes_of(0, 0, 0, 0)).error().code(), ErrorCode::DataLoss);
    EXPECT_EQ(decode_integer(oid::kInt8, {}).error().code(), ErrorCode::DataLoss);
    EXPECT_EQ(decode_real(oid::kFloat8, bytes_of(0, 0, 0, 0)).error().code(), ErrorCode::DataLoss);
    EXPECT_EQ(decode_real(oid::kFloat4, bytes_of(0, 0, 0, 0, 0)).error().code(),
              ErrorCode::DataLoss);
    EXPECT_EQ(decode_boolean(oid::kBool, bytes_of(2)).error().code(), ErrorCode::DataLoss);
    EXPECT_EQ(decode_boolean(oid::kBool, {}).error().code(), ErrorCode::DataLoss);
    EXPECT_EQ(decode_time(oid::kTimestampTz, bytes_of(0)).error().code(), ErrorCode::DataLoss);
    // +infinity của timestamptz.
    EXPECT_EQ(
        decode_time(oid::kTimestampTz, bytes_of(0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF))
            .error()
            .code(),
        ErrorCode::OutOfRange);
}

// Ghi rồi đọc lại qua cùng OID cho đúng giá trị: Param và decode_* là hai nửa của một định dạng.
TEST(Decode, ReadsBackWhatParamWrites) {
    for (const i64 value : {kMin, i64{-1}, i64{0}, i64{42}, kMax}) {
        const Param param = Param::integer(value);
        EXPECT_EQ(decode_integer(param.type(), param.wire()), value);
    }
    for (const f64 value : {-0.0, 0.1, -1e308, std::numeric_limits<f64>::denorm_min()}) {
        const Param param = Param::real(value);
        const Result<f64> read = decode_real(param.type(), param.wire());
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(std::bit_cast<u64>(*read), std::bit_cast<u64>(value));
    }
    for (const i64 unix : {i64{0}, i64{-1}, i64{1'700'000'000'123'456}, kMax}) {
        const Param param = Param::time(core::WallTime::from_unix_microseconds(unix));
        const Result<core::WallTime> read = decode_time(param.type(), param.wire());
        ASSERT_TRUE(read.has_value()) << unix;
        EXPECT_EQ(read->unix_microseconds(), unix);
    }
    const Param yes = Param::boolean(true);
    EXPECT_EQ(decode_boolean(yes.type(), yes.wire()), true);
    const Param text = Param::text("xin chào");
    EXPECT_EQ(decode_text(text.type(), text.wire()), "xin chào");
}

}  // namespace
}  // namespace orion::db
