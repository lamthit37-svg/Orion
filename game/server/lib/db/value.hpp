#pragma once

// Giá trị PostgreSQL ở định dạng nhị phân (ADR 0013): Param mã hoá một tham số của truy vấn, các
// hàm decode_* giải mã một ô của kết quả. server_db luôn gửi và nhận ở định dạng nhị phân: số thực
// đi nguyên từng bit, chuỗi và byte không cần NUL, không có bước đổi sang chữ rồi đọc lại.
//
// Định dạng của mỗi kiểu là cặp hàm send, recv của kiểu đó trong mã nguồn PostgreSQL
// (src/backend/utils/adt/): số nguyên và số thực big-endian; bool một byte 0 hay 1; text, varchar
// và name là byte của chuỗi theo client_encoding, mà kết nối của server_db luôn đặt là UTF8; bytea
// là byte thô; timestamptz là số micro giây kiểu i64 tính từ 2000-01-01 00:00:00 UTC.
//
// Ô đọc từ DB là dữ liệu từ ngoài (CLAUDE.md X.5): sai kiểu, sai độ dài hay giá trị lạ là lỗi trả
// về, không phải assert. Các hàm decode_* là parser, có fuzz target tests/fuzz/db_value.cpp (X.4).

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace orion::db {

// OID của các kiểu dựng sẵn mà server_db đọc và ghi (catalog pg_type). OID của kiểu dựng sẵn không
// đổi giữa các bản PostgreSQL.
namespace oid {
inline constexpr u32 kUnknown = 0;  // tham số NULL: server tự suy kiểu từ ngữ cảnh
inline constexpr u32 kBool = 16;
inline constexpr u32 kBytea = 17;
inline constexpr u32 kName = 19;
inline constexpr u32 kInt8 = 20;
inline constexpr u32 kInt2 = 21;
inline constexpr u32 kInt4 = 23;
inline constexpr u32 kText = 25;
inline constexpr u32 kFloat4 = 700;
inline constexpr u32 kFloat8 = 701;
inline constexpr u32 kVarchar = 1043;
inline constexpr u32 kTimestampTz = 1184;
}  // namespace oid

// Mốc thời gian của PostgreSQL, 2000-01-01 00:00:00 UTC, tính bằng micro giây từ Unix epoch.
inline constexpr i64 kPostgresEpochUnixMicros = 946'684'800'000'000;

// Thời điểm theo mốc của PostgreSQL. Thời điểm sớm tới mức phép trừ tràn i64 thành i64 nhỏ nhất
// cộng 1: một giá trị hữu hạn ngoài khoảng mà server từ chối (SQLSTATE 22008), không bao giờ thành
// i64 nhỏ nhất, thứ PostgreSQL hiểu là -infinity.
[[nodiscard]] i64 to_postgres_micros(core::WallTime time) noexcept;
// Ngược lại. Lỗi OutOfRange với -infinity, +infinity và thời điểm không vừa WallTime.
[[nodiscard]] Result<core::WallTime> from_postgres_micros(i64 micros) noexcept;

// Một tham số của truy vấn, đã mã hoá sẵn. Số nằm ngay trong Param; text và bytes chỉ trỏ vào bộ
// nhớ của bên gọi, nên vùng nhớ đó phải sống tới khi truy vấn trả về: đừng truyền chuỗi tạm.
class Param {
public:
    [[nodiscard]] static Param null() noexcept;
    [[nodiscard]] static Param boolean(bool value) noexcept;
    [[nodiscard]] static Param integer(i64 value) noexcept;  // int8
    [[nodiscard]] static Param real(f64 value) noexcept;     // float8
    [[nodiscard]] static Param text(std::string_view value) noexcept;
    [[nodiscard]] static Param bytes(std::span<const std::byte> value) noexcept;  // bytea
    [[nodiscard]] static Param time(core::WallTime value) noexcept;               // timestamptz

    // OID của kiểu; oid::kUnknown với NULL.
    [[nodiscard]] u32 type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return null_; }
    // Giá trị ở định dạng nhị phân; rỗng với NULL.
    [[nodiscard]] std::span<const std::byte> wire() const noexcept;

private:
    Param(u32 type, bool null, usize inline_size, std::span<const std::byte> external) noexcept;

    u32 type_ = oid::kUnknown;
    bool null_ = true;
    u8 inline_size_ = 0;
    std::array<std::byte, 8> inline_{};
    std::span<const std::byte> external_;
};

// Giải mã một ô không NULL theo OID kiểu của cột. Lỗi: InvalidArgument khi cột có kiểu khác (detail
// là OID của cột), DataLoss khi độ dài hay nội dung không đúng định dạng của kiểu (detail là độ
// dài).
[[nodiscard]] Result<bool> decode_boolean(u32 type, std::span<const std::byte> bytes) noexcept;
// int2, int4 hay int8.
[[nodiscard]] Result<i64> decode_integer(u32 type, std::span<const std::byte> bytes) noexcept;
// float4 hay float8. float8 giữ đúng từng bit; float4 đổi sang f64 đúng tuyệt đối, NaN vẫn là NaN
// (NaN báo hiệu thành NaN yên cùng payload, IEEE 754) và vô cực vẫn là vô cực.
[[nodiscard]] Result<f64> decode_real(u32 type, std::span<const std::byte> bytes) noexcept;
// text, varchar hay name. View trỏ vào chính `bytes`.
[[nodiscard]] Result<std::string_view> decode_text(u32 type,
                                                   std::span<const std::byte> bytes) noexcept;
// bytea. Span trỏ vào chính `bytes`.
[[nodiscard]] Result<std::span<const std::byte>> decode_bytes(
    u32 type, std::span<const std::byte> bytes) noexcept;
// timestamptz. Lỗi thêm: OutOfRange với ±infinity và thời điểm không vừa WallTime.
[[nodiscard]] Result<core::WallTime> decode_time(u32 type,
                                                 std::span<const std::byte> bytes) noexcept;

}  // namespace orion::db
