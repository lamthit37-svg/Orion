#pragma once

// orion_migrate (docs/formats/migrations.md, CLAUDE.md X.14): đọc db/migrations/, kiểm lịch sử
// trong bảng schema_migrations, áp các migration chưa áp, mỗi migration một transaction.
//
// Lỗi mang theo tệp hay phiên bản gây ra nó (Failure): người đọc là người vận hành chạy công cụ,
// cần biết sửa tệp nào. crypto::initialize() phải đã thành công trước parse và load, vì chúng băm
// nội dung tệp.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/hash.hpp"
#include "game/server/lib/db/db.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orion::migrate {

// Phiên bản lớn nhất: đúng bốn chữ số.
inline constexpr u32 kMaxVersion = 9'999;
// Cỡ tối đa của một tệp migration: thay đổi schema không cần tới cỡ này, còn dump dữ liệu không
// thuộc migration.
inline constexpr usize kMaxMigrationBytes = usize{16} << 20U;

struct Migration {
    u32 version = 0;
    std::string name;  // phần <tên> của NNNN_<tên>.sql
    std::string sql;
    crypto::Hash checksum;

    // "NNNN_<tên>.sql".
    [[nodiscard]] std::string file_name() const;
};

// Lỗi kèm nơi gây ra nó (tên tệp, đường dẫn thư mục, hay bảng schema_migrations), và thông điệp
// của server khi lỗi đến từ DB (db::Connection::last_error).
struct Failure {
    Error error;
    std::string subject;
    std::string message;
};

template <class T>
using Outcome = std::expected<T, Failure>;

// Kiểm tên và nội dung một tệp theo mục "Tệp" của migrations.md. Lỗi: InvalidArgument.
[[nodiscard]] Outcome<Migration> parse(std::string_view file_name, std::string content);

// Đọc mọi tệp .sql trong `dir` (không đệ quy), bỏ qua tệp đuôi khác; trả theo phiên bản tăng dần,
// đã kiểm phiên bản bắt đầu từ 1, liên tiếp, không trùng. Lỗi: NotFound (thư mục không có), Io,
// InvalidArgument, ResourceExhausted (tệp lớn hơn kMaxMigrationBytes).
[[nodiscard]] Outcome<std::vector<Migration>> load(const std::filesystem::path& dir);

struct Options {
    // Hạn của mỗi migration, và của mỗi bước kiểm (tạo bảng, đọc lịch sử).
    core::Duration timeout = core::Duration::seconds(600);
};

struct Status {
    u32 applied = 0;  // số migration đã áp trong DB, sau lời gọi
    u32 pending = 0;  // số migration có trong code mà DB chưa áp
};

// Kiểm lịch sử của DB với `migrations` (bước 1 và 3); không ghi gì, kể cả bảng schema_migrations.
// `clock` phải là đồng hồ của `connection`. Lỗi: FailedPrecondition khi lịch sử không khớp code,
// và lỗi của server_db.
[[nodiscard]] Outcome<Status> check(db::Connection& connection,
                                    std::span<const Migration> migrations,
                                    const core::MonotonicClock& clock, const Options& options = {});

// Tạo schema_migrations nếu chưa có, kiểm lịch sử, rồi áp lần lượt mọi migration chưa áp (bước
// 2–6). `on_applied` được gọi sau mỗi migration đã commit, nên vẫn biết cái nào đã áp khi một cái
// sau lỗi. Lỗi như check, thêm lỗi của chính migration (subject là tên tệp của nó).
[[nodiscard]] Outcome<Status> apply(db::Connection& connection,
                                    std::span<const Migration> migrations,
                                    const core::MonotonicClock& clock, const Options& options = {},
                                    const std::function<void(const Migration&)>& on_applied = {});

// Áp đúng một migration trong transaction của nó (bước 4). Trả false khi nó đã được áp, bởi lần
// chạy này hay lần chạy khác, với cùng tên và checksum. Tiền điều kiện: bảng schema_migrations đã
// có (apply tạo nó). Dùng bởi apply; public để test được nhánh "lần chạy khác vừa áp".
[[nodiscard]] Outcome<bool> apply_one(db::Connection& connection, const Migration& migration,
                                      core::MonoTime deadline);

}  // namespace orion::migrate
