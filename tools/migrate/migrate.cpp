#include "tools/migrate/migrate.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/db/value.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace orion::migrate {
namespace {

// Khoá advisory của mọi transaction do orion_migrate mở: "ORIONMIG" theo ASCII.
constexpr i64 kLockKey = 0x4F52'494F'4E4D'4947;
constexpr std::string_view kSuffix = ".sql";
constexpr usize kVersionDigits = 4;
constexpr usize kMaxNameLength = 60;
constexpr std::string_view kHistoryTable = "schema_migrations";

[[nodiscard]] std::unexpected<Failure> failure(const ErrorCode code, const ErrorContext context,
                                               std::string subject, const i64 detail = 0) {
    return std::unexpected(Failure{Error{code, context, detail}, std::move(subject), {}});
}

// Lỗi từ server_db: kèm thông điệp của server để người vận hành biết câu nào hỏng.
[[nodiscard]] std::unexpected<Failure> db_failure(const Error& error,
                                                  const db::Connection& connection,
                                                  std::string subject) {
    return std::unexpected(
        Failure{error, std::move(subject), std::string(connection.last_error())});
}

[[nodiscard]] bool is_name_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

struct FileName {
    u32 version = 0;
    std::string_view name;
};

// "NNNN_<tên>.sql" thành phiên bản và tên; nullopt khi sai dạng.
[[nodiscard]] std::optional<FileName> split_file_name(std::string_view file) {
    if (!file.ends_with(kSuffix)) {
        return std::nullopt;
    }
    file.remove_suffix(kSuffix.size());
    if (file.size() < kVersionDigits + 2 || file[kVersionDigits] != '_') {
        return std::nullopt;
    }
    u32 version = 0;
    for (const char c : file.substr(0, kVersionDigits)) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        version = (version * 10) + static_cast<u32>(c - '0');
    }
    const std::string_view name = file.substr(kVersionDigits + 1);
    if (name.size() > kMaxNameLength || !std::ranges::all_of(name, is_name_char)) {
        return std::nullopt;
    }
    return FileName{.version = version, .name = name};
}

[[nodiscard]] Outcome<std::string> read_file(const std::filesystem::path& path,
                                             const std::string& subject) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return failure(ErrorCode::Io, "migrate: không đọc được cỡ tệp", subject, error.value());
    }
    if (size > kMaxMigrationBytes) {
        return failure(ErrorCode::ResourceExhausted, "migrate: tệp migration lớn hơn 16 MiB",
                       subject, static_cast<i64>(size));
    }
    std::ifstream in(path, std::ios::binary);
    std::string content(static_cast<usize>(size), '\0');
    if (!in || !in.read(content.data(), static_cast<std::streamsize>(content.size()))) {
        return failure(ErrorCode::Io, "migrate: không đọc được tệp", subject);
    }
    return content;
}

// Tên tệp theo UTF-8 trên mọi nền tảng (path::string() trên Windows đổi theo code page ANSI).
[[nodiscard]] std::string utf8_name(const std::filesystem::path& path) {
    const std::u8string name = path.filename().u8string();
    return {name.begin(), name.end()};
}

// Mọi transaction của orion_migrate lấy khoá này trước, nên hai lần chạy cùng lúc nối đuôi nhau.
[[nodiscard]] Result<void> lock(db::Transaction& transaction) {
    const std::array key{db::Param::integer(kLockKey)};
    const Result<db::Rows> locked = transaction.execute("SELECT pg_advisory_xact_lock($1)", key);
    if (!locked) {
        return std::unexpected(locked.error());
    }
    return {};
}

[[nodiscard]] Result<std::string> transaction_id(db::Transaction& transaction) {
    const Result<db::Rows> rows = transaction.execute("SELECT pg_current_xact_id()::text");
    if (!rows) {
        return std::unexpected(rows.error());
    }
    const Result<std::string_view> id = rows->text(0, 0);
    if (!id) {
        return std::unexpected(id.error());
    }
    return std::string(*id);
}

struct Applied {
    u32 version = 0;
    std::string name;
    crypto::Hash checksum;
};

// Đọc một dòng của schema_migrations: dữ liệu từ DB, nên mọi giá trị lạ là lỗi, không phải assert.
[[nodiscard]] Result<Applied> read_applied(const db::Rows& rows, const usize row) {
    const Result<i64> version = rows.integer(row, 0);
    const Result<std::string_view> name = rows.text(row, 1);
    const Result<std::span<const std::byte>> checksum = rows.bytes(row, 2);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!checksum) {
        return std::unexpected(checksum.error());
    }
    if (*version < 1 || *version > i64{kMaxVersion} || checksum->size() != crypto::kHashSize) {
        return fail(ErrorCode::DataLoss, "migrate: schema_migrations có dòng sai định dạng",
                    *version);
    }
    Applied applied{
        .version = static_cast<u32>(*version), .name = std::string(*name), .checksum = {}};
    std::ranges::copy(*checksum, applied.checksum.bytes.begin());
    return applied;
}

[[nodiscard]] Outcome<std::vector<Applied>> read_history(db::Connection& connection,
                                                         const core::MonoTime deadline) {
    const std::string subject(kHistoryTable);
    const Result<db::Rows> exists =
        connection.execute("SELECT to_regclass('schema_migrations') IS NOT NULL", {}, deadline);
    if (!exists) {
        return db_failure(exists.error(), connection, subject);
    }
    const Result<bool> present = exists->boolean(0, 0);
    if (!present) {
        return std::unexpected(Failure{present.error(), subject, {}});
    }
    std::vector<Applied> history;
    if (!*present) {
        return history;
    }
    const Result<db::Rows> rows = connection.execute(
        "SELECT version::int8, name, checksum FROM schema_migrations ORDER BY version", {},
        deadline);
    if (!rows) {
        return db_failure(rows.error(), connection, subject);
    }
    for (usize row = 0; row < rows->size(); ++row) {
        Result<Applied> applied = read_applied(*rows, row);
        if (!applied) {
            return std::unexpected(Failure{applied.error(), subject, {}});
        }
        history.push_back(std::move(*applied));
    }
    return history;
}

// Lịch sử phải là 1..k liên tiếp, mỗi dòng khớp tên và checksum của tệp cùng phiên bản.
[[nodiscard]] Outcome<u32> verify(const std::vector<Applied>& history,
                                  const std::span<const Migration> migrations) {
    for (usize i = 0; i < history.size(); ++i) {
        const Applied& row = history[i];
        if (row.version != i + 1) {
            return failure(ErrorCode::FailedPrecondition, "migrate: lịch sử trong DB có lỗ",
                           std::string(kHistoryTable), static_cast<i64>(i + 1));
        }
        if (i >= migrations.size()) {
            return failure(ErrorCode::FailedPrecondition,
                           "migrate: DB đã áp migration mà code không có",
                           std::format("{:04}_{}.sql", row.version, row.name), row.version);
        }
        const Migration& file = migrations[i];
        if (row.name != file.name || !crypto::equal_constant_time(row.checksum, file.checksum)) {
            return failure(ErrorCode::FailedPrecondition,
                           "migrate: migration đã áp bị đổi tên hay sửa nội dung", file.file_name(),
                           row.version);
        }
    }
    return static_cast<u32>(history.size());
}

[[nodiscard]] Outcome<void> bootstrap(db::Connection& connection, const core::MonoTime deadline) {
    const std::string subject(kHistoryTable);
    Result<db::Transaction> transaction = connection.begin(db::Isolation::ReadCommitted, deadline);
    if (!transaction) {
        return db_failure(transaction.error(), connection, subject);
    }
    if (const Result<void> locked = lock(*transaction); !locked) {
        return db_failure(locked.error(), connection, subject);
    }
    const Result<db::Rows> created = transaction->execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "version integer PRIMARY KEY CHECK (version > 0), "
        "name text NOT NULL, "
        "checksum bytea NOT NULL CHECK (octet_length(checksum) = 32), "
        "applied_at timestamptz NOT NULL DEFAULT now())");
    if (!created) {
        return db_failure(created.error(), connection, subject);
    }
    if (const Result<void> committed = transaction->commit(); !committed) {
        return db_failure(committed.error(), connection, subject);
    }
    return {};
}

}  // namespace

std::string Migration::file_name() const {
    return std::format("{:04}_{}.sql", version, name);
}

Outcome<Migration> parse(const std::string_view file_name, std::string content) {
    const std::string subject(file_name);
    const std::optional<FileName> parts = split_file_name(file_name);
    if (!parts || parts->name.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "migrate: tên tệp không theo dạng NNNN_<tên>.sql", subject);
    }
    if (parts->version == 0) {
        return failure(ErrorCode::InvalidArgument, "migrate: phiên bản bắt đầu từ 0001", subject);
    }
    if (content.empty()) {
        return failure(ErrorCode::InvalidArgument, "migrate: migration rỗng", subject);
    }
    if (content.find('\0') != std::string::npos) {
        return failure(ErrorCode::InvalidArgument, "migrate: migration có byte 0", subject);
    }
    if (!core::is_valid_utf8(content)) {
        return failure(ErrorCode::InvalidArgument, "migrate: migration không phải UTF-8", subject);
    }
    Migration migration;
    migration.version = parts->version;
    migration.name = std::string(parts->name);
    migration.checksum = crypto::hash(std::as_bytes(std::span(content)));
    migration.sql = std::move(content);
    return migration;
}

Outcome<std::vector<Migration>> load(const std::filesystem::path& dir) {
    const std::string where = utf8_name(dir);
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        return failure(ErrorCode::NotFound, "migrate: không có thư mục migration", where);
    }
    std::vector<Migration> migrations;
    std::filesystem::directory_iterator entry(dir, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        const std::string file = utf8_name(entry->path());
        if (!file.ends_with(kSuffix)) {
            continue;
        }
        if (!entry->is_regular_file(error)) {
            return failure(ErrorCode::InvalidArgument, "migrate: tệp .sql không phải tệp thường",
                           file);
        }
        Outcome<std::string> content = read_file(entry->path(), file);
        if (!content) {
            return std::unexpected(content.error());
        }
        Outcome<Migration> migration = parse(file, std::move(*content));
        if (!migration) {
            return std::unexpected(migration.error());
        }
        migrations.push_back(std::move(*migration));
    }
    if (error) {
        return failure(ErrorCode::Io, "migrate: không đọc được thư mục migration", where,
                       error.value());
    }
    std::ranges::sort(migrations, {}, &Migration::version);
    for (usize i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version == i + 1) {
            continue;
        }
        if (i > 0 && migrations[i].version == migrations[i - 1].version) {
            return failure(ErrorCode::InvalidArgument, "migrate: hai migration trùng phiên bản",
                           migrations[i].file_name(), migrations[i].version);
        }
        return failure(ErrorCode::InvalidArgument, "migrate: phiên bản migration có lỗ",
                       migrations[i].file_name(), static_cast<i64>(i + 1));
    }
    return migrations;
}

Outcome<Status> check(db::Connection& connection, const std::span<const Migration> migrations,
                      const core::MonotonicClock& clock, const Options& options) {
    const Outcome<std::vector<Applied>> history =
        read_history(connection, clock.now() + options.timeout);
    if (!history) {
        return std::unexpected(history.error());
    }
    const Outcome<u32> applied = verify(*history, migrations);
    if (!applied) {
        return std::unexpected(applied.error());
    }
    return Status{.applied = *applied, .pending = static_cast<u32>(migrations.size()) - *applied};
}

Outcome<Status> apply(db::Connection& connection, const std::span<const Migration> migrations,
                      const core::MonotonicClock& clock, const Options& options,
                      const std::function<void(const Migration&)>& on_applied) {
    if (const Outcome<void> created = bootstrap(connection, clock.now() + options.timeout);
        !created) {
        return std::unexpected(created.error());
    }
    const Outcome<Status> status = check(connection, migrations, clock, options);
    if (!status) {
        return status;
    }
    for (usize i = status->applied; i < migrations.size(); ++i) {
        const Migration& migration = migrations[i];
        const Outcome<bool> applied =
            apply_one(connection, migration, clock.now() + options.timeout);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        if (*applied && on_applied) {
            on_applied(migration);
        }
    }
    return Status{.applied = static_cast<u32>(migrations.size()), .pending = 0};
}

Outcome<bool> apply_one(db::Connection& connection, const Migration& migration,
                        const core::MonoTime deadline) {
    const std::string subject = migration.file_name();
    Result<db::Transaction> transaction = connection.begin(db::Isolation::ReadCommitted, deadline);
    if (!transaction) {
        return db_failure(transaction.error(), connection, subject);
    }
    if (const Result<void> locked = lock(*transaction); !locked) {
        return db_failure(locked.error(), connection, subject);
    }
    // Trong lúc chờ khoá, một lần chạy khác có thể đã áp đúng migration này.
    const std::array version{db::Param::integer(migration.version)};
    const Result<db::Rows> existing = transaction->execute(
        "SELECT version::int8, name, checksum FROM schema_migrations WHERE version = $1", version);
    if (!existing) {
        return db_failure(existing.error(), connection, subject);
    }
    if (existing->size() > 0) {
        const Result<Applied> applied = read_applied(*existing, 0);
        if (!applied || applied->name != migration.name ||
            !crypto::equal_constant_time(applied->checksum, migration.checksum)) {
            return failure(ErrorCode::FailedPrecondition,
                           "migrate: migration đã áp bị đổi tên hay sửa nội dung", subject,
                           migration.version);
        }
        return false;
    }
    const Result<std::string> before = transaction_id(*transaction);
    if (!before) {
        return db_failure(before.error(), connection, subject);
    }
    if (const Result<void> ran = transaction->execute_script(migration.sql); !ran) {
        return db_failure(ran.error(), connection, subject);
    }
    // Script tự COMMIT hay ROLLBACK thì câu sau chạy trong một transaction khác, với id khác.
    const Result<std::string> after = transaction_id(*transaction);
    if (!after) {
        return db_failure(after.error(), connection, subject);
    }
    if (*after != *before) {
        return failure(ErrorCode::FailedPrecondition,
                       "migrate: migration tự kết thúc transaction của nó", subject,
                       migration.version);
    }
    const std::array row{db::Param::integer(migration.version), db::Param::text(migration.name),
                         db::Param::bytes(migration.checksum.view())};
    const Result<db::Rows> recorded = transaction->execute(
        "INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)", row);
    if (!recorded) {
        return db_failure(recorded.error(), connection, subject);
    }
    if (const Result<void> committed = transaction->commit(); !committed) {
        return db_failure(committed.error(), connection, subject);
    }
    return true;
}

}  // namespace orion::migrate
