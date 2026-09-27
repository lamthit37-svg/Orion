#include "game/server/lib/db/value.hpp"

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <bit>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace orion::db {
namespace {

// PostgreSQL gửi số theo thứ tự byte mạng: đọc little-endian rồi đảo byte là big-endian, trên mọi
// máy.
template <std::integral T>
[[nodiscard]] T load_be(const std::span<const std::byte, sizeof(T)> bytes) noexcept {
    return std::byteswap(core::load_le<T>(bytes));
}

template <std::integral T>
void store_be(const std::span<std::byte, sizeof(T)> bytes, const T value) noexcept {
    core::store_le<T>(bytes, std::byteswap(value));
}

[[nodiscard]] std::unexpected<Error> wrong_type(const u32 type) noexcept {
    return fail(ErrorCode::InvalidArgument, "db: cột có kiểu khác kiểu được đọc", type);
}

[[nodiscard]] std::unexpected<Error> malformed(const std::span<const std::byte> bytes) noexcept {
    return fail(ErrorCode::DataLoss, "db: giá trị nhị phân sai định dạng của kiểu",
                static_cast<i64>(bytes.size()));
}

// Số nguyên có dấu rộng đúng sizeof(T) byte, như int2send, int4send, int8send ghi.
template <std::integral T>
[[nodiscard]] Result<i64> sized_integer(const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != sizeof(T)) {
        return malformed(bytes);
    }
    return load_be<T>(bytes.first<sizeof(T)>());
}

// PostgreSQL dùng i64 nhỏ nhất và lớn nhất cho -infinity và +infinity của timestamptz.
constexpr i64 kMinusInfinity = std::numeric_limits<i64>::min();
constexpr i64 kPlusInfinity = std::numeric_limits<i64>::max();

}  // namespace

i64 to_postgres_micros(const core::WallTime time) noexcept {
    const i64 unix = time.unix_microseconds();
    // unix - mốc tràn khi unix < i64 nhỏ nhất + mốc; bằng đúng thì ra -infinity. Cả hai đều xa
    // ngoài khoảng của PostgreSQL (4713 TCN), nên thay bằng một giá trị hữu hạn server sẽ từ chối.
    if (unix <= kMinusInfinity + kPostgresEpochUnixMicros) {
        return kMinusInfinity + 1;
    }
    return unix - kPostgresEpochUnixMicros;
}

Result<core::WallTime> from_postgres_micros(const i64 micros) noexcept {
    if (micros == kMinusInfinity || micros == kPlusInfinity) {
        return fail(ErrorCode::OutOfRange, "db: timestamptz là vô cực", micros);
    }
    if (micros > kPlusInfinity - kPostgresEpochUnixMicros) {
        return fail(ErrorCode::OutOfRange, "db: timestamptz không vừa WallTime", micros);
    }
    return core::WallTime::from_unix_microseconds(micros + kPostgresEpochUnixMicros);
}

Param::Param(const u32 type, const bool null, const usize inline_size,
             const std::span<const std::byte> external) noexcept
    : type_(type), null_(null), inline_size_(static_cast<u8>(inline_size)), external_(external) {}

Param Param::null() noexcept {
    return {oid::kUnknown, true, 0, {}};
}

Param Param::boolean(const bool value) noexcept {
    Param param{oid::kBool, false, 1, {}};
    param.inline_[0] = value ? std::byte{1} : std::byte{0};
    return param;
}

Param Param::integer(const i64 value) noexcept {
    Param param{oid::kInt8, false, sizeof(i64), {}};
    store_be<i64>(param.inline_, value);
    return param;
}

Param Param::real(const f64 value) noexcept {
    Param param{oid::kFloat8, false, sizeof(f64), {}};
    store_be<u64>(param.inline_, std::bit_cast<u64>(value));
    return param;
}

Param Param::text(const std::string_view value) noexcept {
    return {oid::kText, false, 0, std::as_bytes(std::span(value))};
}

Param Param::bytes(const std::span<const std::byte> value) noexcept {
    return {oid::kBytea, false, 0, value};
}

Param Param::time(const core::WallTime value) noexcept {
    Param param{oid::kTimestampTz, false, sizeof(i64), {}};
    store_be<i64>(param.inline_, to_postgres_micros(value));
    return param;
}

std::span<const std::byte> Param::wire() const noexcept {
    if (inline_size_ != 0) {
        return std::span<const std::byte>(inline_).first(inline_size_);
    }
    return external_;
}

Result<bool> decode_boolean(const u32 type, const std::span<const std::byte> bytes) noexcept {
    if (type != oid::kBool) {
        return wrong_type(type);
    }
    // boolsend của PostgreSQL chỉ gửi 0 hay 1.
    if (bytes.size() != 1 || std::to_integer<u8>(bytes[0]) > 1) {
        return malformed(bytes);
    }
    return bytes[0] == std::byte{1};
}

Result<i64> decode_integer(const u32 type, const std::span<const std::byte> bytes) noexcept {
    switch (type) {
        case oid::kInt2:
            return sized_integer<i16>(bytes);
        case oid::kInt4:
            return sized_integer<i32>(bytes);
        case oid::kInt8:
            return sized_integer<i64>(bytes);
        default:
            return wrong_type(type);
    }
}

Result<f64> decode_real(const u32 type, const std::span<const std::byte> bytes) noexcept {
    if (type == oid::kFloat4) {
        if (bytes.size() != sizeof(f32)) {
            return malformed(bytes);
        }
        // f32 sang f64 là phép đổi đúng tuyệt đối; NaN báo hiệu thành NaN yên (IEEE 754).
        return static_cast<f64>(std::bit_cast<f32>(load_be<u32>(bytes.first<sizeof(u32)>())));
    }
    if (type == oid::kFloat8) {
        if (bytes.size() != sizeof(f64)) {
            return malformed(bytes);
        }
        return std::bit_cast<f64>(load_be<u64>(bytes.first<sizeof(u64)>()));
    }
    return wrong_type(type);
}

Result<std::string_view> decode_text(const u32 type,
                                     const std::span<const std::byte> bytes) noexcept {
    if (type != oid::kText && type != oid::kVarchar && type != oid::kName) {
        return wrong_type(type);
    }
    return core::as_chars(bytes);
}

Result<std::span<const std::byte>> decode_bytes(const u32 type,
                                                const std::span<const std::byte> bytes) noexcept {
    if (type != oid::kBytea) {
        return wrong_type(type);
    }
    return bytes;
}

Result<core::WallTime> decode_time(const u32 type,
                                   const std::span<const std::byte> bytes) noexcept {
    if (type != oid::kTimestampTz) {
        return wrong_type(type);
    }
    if (bytes.size() != sizeof(i64)) {
        return malformed(bytes);
    }
    return from_postgres_micros(load_be<i64>(bytes.first<sizeof(i64)>()));
}

}  // namespace orion::db
