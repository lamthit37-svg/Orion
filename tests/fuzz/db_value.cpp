// Fuzz các hàm decode_* của game/server/lib/db (CLAUDE.md X.4): ô đọc từ DB là dữ liệu từ ngoài
// (X.5). Byte đầu chọn OID của cột, trong các kiểu server_db biết cùng vài OID lạ; phần còn lại là
// giá trị. Mọi decode_* trả giá trị hay đúng một trong các mã lỗi đã hứa (value.hpp), không bao giờ
// UB. Giá trị đọc được thì Param cùng kiểu ghi lại ra đúng những byte đó: đọc và ghi là hai nửa của
// một định dạng một-một.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/value.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

using orion::ErrorCode;
using orion::f32;
using orion::f64;
using orion::i64;
using orion::u32;
namespace oid = orion::db::oid;

constexpr std::array kTypes{
    oid::kUnknown,
    oid::kBool,
    oid::kBytea,
    oid::kName,
    oid::kInt8,
    oid::kInt2,
    oid::kInt4,
    oid::kText,
    oid::kFloat4,
    oid::kFloat8,
    oid::kVarchar,
    oid::kTimestampTz,
    u32{1'114},  // timestamp không múi giờ: cùng định dạng, khác kiểu
    std::numeric_limits<u32>::max(),
};

template <class T>
void check_error(const orion::Result<T>& result) {
    if (result.has_value()) {
        return;
    }
    const ErrorCode code = result.error().code();
    ORION_VERIFY(code == ErrorCode::InvalidArgument || code == ErrorCode::DataLoss ||
                     code == ErrorCode::OutOfRange,
                 "mã lỗi ngoài hợp đồng của value.hpp: {}", code);
}

void check_rewrite(const orion::db::Param& param, const std::span<const std::byte> value) {
    ORION_VERIFY(std::ranges::equal(param.wire(), value), "ghi lại không ra đúng byte đã đọc");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orion::db::Param;
    if (size == 0) {
        return 0;
    }
    const u32 type = kTypes[data[0] % kTypes.size()];
    const std::span<const std::byte> value = std::as_bytes(std::span(data + 1, size - 1));

    const orion::Result<bool> boolean = orion::db::decode_boolean(type, value);
    check_error(boolean);
    if (boolean) {
        check_rewrite(Param::boolean(*boolean), value);
    }
    const orion::Result<i64> integer = orion::db::decode_integer(type, value);
    check_error(integer);
    if (integer && type == oid::kInt8) {
        check_rewrite(Param::integer(*integer), value);
    }
    const orion::Result<f64> real = orion::db::decode_real(type, value);
    check_error(real);
    if (real && type == oid::kFloat8) {
        check_rewrite(Param::real(*real), value);
    }
    if (real && type == oid::kFloat4) {
        // f32 sang f64 rồi về f32 giữ đúng từng bit, trừ NaN báo hiệu: phép đổi kiểu biến nó thành
        // NaN yên cùng payload (IEEE 754), nên với NaN chỉ kiểm kết quả vẫn là NaN.
        u32 bits = 0;
        for (const std::byte b : value) {
            bits = (bits << 8U) | std::to_integer<u32>(b);
        }
        const auto back = std::bit_cast<u32>(static_cast<f32>(*real));
        const auto is_nan = [](const u32 x) {
            return (x & 0x7F80'0000U) == 0x7F80'0000U && (x & 0x007F'FFFFU) != 0;
        };
        ORION_VERIFY(is_nan(bits) ? is_nan(back) : back == bits, "float4 đổi giá trị khi qua f64");
    }
    const orion::Result<std::string_view> text = orion::db::decode_text(type, value);
    check_error(text);
    if (text) {
        check_rewrite(Param::text(*text), value);
    }
    const orion::Result<std::span<const std::byte>> bytes = orion::db::decode_bytes(type, value);
    check_error(bytes);
    if (bytes) {
        check_rewrite(Param::bytes(*bytes), value);
    }
    const orion::Result<orion::core::WallTime> time = orion::db::decode_time(type, value);
    check_error(time);
    if (time) {
        check_rewrite(Param::time(*time), value);
    }
    return 0;
}
