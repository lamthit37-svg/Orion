// Bọc libpq: mọi lời gọi đi qua API bất đồng bộ (PQconnectStartParams, PQsendQueryParams,
// PQconsumeInput) trên kết nối không chặn, và chỉ chờ socket bằng detail::wait_socket với hạn còn
// lại, nên không lời gọi nào vượt mốc hạn của bên gọi (CLAUDE.md X.14). PGresult được giữ bằng
// unique_ptr với deleter riêng (X.7).

#include "game/server/lib/db/db.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"
#include "game/server/lib/db/detail/socket_wait.hpp"
#include "game/server/lib/db/detail/sqlstate.hpp"
#include "game/server/lib/db/value.hpp"

#include <libpq-fe.h>
#include <postgres_ext.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace orion::db {
namespace {

// Một giá trị của PostgreSQL tối đa 1 GiB.
constexpr usize kMaxParamBytes = usize{1} << 30U;
// client_connection_check_interval có từ PostgreSQL 14.
constexpr int kMinServerVersion = 140'000;
// Thông điệp lỗi giữ lại cho last_error().
constexpr usize kMaxErrorMessage = 255;

struct ResultDeleter {
    void operator()(PGresult* result) const noexcept { PQclear(result); }
};
using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;

[[nodiscard]] std::string_view view(const char* text) noexcept {
    return text != nullptr ? std::string_view(text) : std::string_view();
}

[[nodiscard]] PGresult* native(void* result) noexcept {
    return static_cast<PGresult*>(result);
}

// NOTICE và WARNING của server (ví dụ "bảng đã có, bỏ qua" của migration): chỉ để log, không đổi
// kết quả của câu. Log toàn cục có thể chưa được đặt (test); khi đó core::log không làm gì.
void receive_notice(void* /*context*/, const PGresult* result) noexcept {
    const std::string_view severity =
        view(PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED));
    const core::LogLevel level =
        severity == "WARNING" ? core::LogLevel::Warn : core::LogLevel::Debug;
    core::log(level, {}, "db: server báo {}: {}", severity,
              view(PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY)));
}

// Số mili giây cho tham số của server, cùng cách làm tròn với hạn chờ socket: hạn dương không thành
// 0 (0 là không giới hạn), hạn quá lớn chặn ở INT_MAX, giới hạn của tham số đó.
[[nodiscard]] std::string_view milliseconds_text(const core::Duration value,
                                                 const std::span<char, 16> out) noexcept {
    const auto result =
        std::to_chars(out.data(), out.data() + out.size(), detail::poll_timeout_ms(value));
    return {out.data(), result.ptr};
}

[[nodiscard]] Sql begin_sql(const Isolation isolation) noexcept {
    switch (isolation) {
        case Isolation::RepeatableRead:
            return "BEGIN ISOLATION LEVEL REPEATABLE READ";
        case Isolation::Serializable:
            return "BEGIN ISOLATION LEVEL SERIALIZABLE";
        case Isolation::ReadCommitted:
            break;
    }
    return "BEGIN ISOLATION LEVEL READ COMMITTED";
}

// Một ô của kết quả: kiểu của cột, byte của giá trị, và có phải NULL không.
struct Cell {
    u32 type = oid::kUnknown;
    std::span<const std::byte> bytes;
    bool null = false;
};

[[nodiscard]] Result<Cell> cell_at(const PGresult* result, const usize row,
                                   const usize column) noexcept {
    // PQntuples và PQnfields trả 0 với kết quả null (Rows đã chuyển đi).
    if (std::cmp_greater_equal(row, PQntuples(result)) ||
        std::cmp_greater_equal(column, PQnfields(result))) {
        return fail(ErrorCode::OutOfRange, "db: ô nằm ngoài bảng kết quả");
    }
    const auto r = static_cast<int>(row);
    const auto c = static_cast<int>(column);
    Cell cell{.type = PQftype(result, c), .bytes = {}, .null = false};
    if (PQgetisnull(result, r, c) != 0) {
        cell.null = true;
        return cell;
    }
    const auto length = static_cast<usize>(PQgetlength(result, r, c));
    cell.bytes = std::as_bytes(std::span(PQgetvalue(result, r, c), length));
    return cell;
}

template <class T>
using Decoder = Result<T> (*)(u32, std::span<const std::byte>) noexcept;

template <class T>
[[nodiscard]] Result<T> read_cell(void* result, const usize row, const usize column,
                                  const Decoder<T> decode) noexcept {
    const Result<Cell> cell = cell_at(native(result), row, column);
    if (!cell) {
        return std::unexpected(cell.error());
    }
    if (cell->null) {
        return fail(ErrorCode::FailedPrecondition, "db: ô là NULL");
    }
    return decode(cell->type, cell->bytes);
}

}  // namespace

// Trạng thái của một kết nối. Không đồng bộ: thuộc luồng đang dùng Connection (db.hpp).
class Connection::Impl {
public:
    Impl(const core::MonotonicClock& clock, const Options& options)
        : clock_(&clock), options_(options), application_name_(options.application_name) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() { close(); }

    [[nodiscard]] Result<void> connect(std::string_view conninfo, core::MonoTime deadline);
    [[nodiscard]] Result<Rows> execute(Sql sql, std::span<const Param> params,
                                       core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<void> script(std::string_view text, core::MonoTime deadline);

    [[nodiscard]] bool connected() const noexcept { return conn_ != nullptr; }
    void close() noexcept {
        if (conn_ != nullptr) {
            PQfinish(conn_);
            conn_ = nullptr;
        }
    }
    // Server còn một transaction mở trên kết nối (câu COMMIT hay ROLLBACK không tới được server):
    // đóng kết nối để server tự ROLLBACK, thay vì trả một kết nối đang dở transaction về pool.
    void leave_clean() noexcept {
        if (conn_ != nullptr && PQtransactionStatus(conn_) != PQTRANS_IDLE) {
            close();
        }
    }

    [[nodiscard]] std::string_view last_error() const noexcept {
        return {error_.data(), error_length_};
    }
    void record(std::string_view message) noexcept;

    [[nodiscard]] const core::MonotonicClock& clock() const noexcept { return *clock_; }
    [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }
    void set_in_transaction(const bool open) noexcept { in_transaction_ = open; }

private:
    [[nodiscard]] Result<void> check_conninfo(const std::string& conninfo) noexcept;
    [[nodiscard]] Result<void> poll_connect(core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<void> prepare_session(core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<void> ready_to_send(core::MonoTime deadline) const noexcept;
    [[nodiscard]] Result<void> wait(detail::Wait what, core::MonoTime deadline) const noexcept;
    [[nodiscard]] Result<void> flush(core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<void> await_result(core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<ResultPtr> receive(core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<Rows> collect(core::MonoTime deadline) noexcept;
    [[nodiscard]] Result<Rows> to_rows(ResultPtr result) noexcept;
    [[nodiscard]] std::unexpected<Error> server_error(const PGresult& result) noexcept;
    // Kết nối hỏng: giữ thông điệp của libpq, đóng kết nối, trả Unavailable.
    [[nodiscard]] std::unexpected<Error> lost(ErrorContext context) noexcept;
    // Lời chờ thất bại khi câu đang dở (quá hạn, lỗi poll): kết nối không dùng lại được nữa.
    [[nodiscard]] std::unexpected<Error> abandon(const Error& error) noexcept;
    void clear_error() noexcept { error_length_ = 0; }

    PGconn* conn_ = nullptr;
    const core::MonotonicClock* clock_;  // không null, sống lâu hơn kết nối (db.hpp)
    Options options_;
    std::string application_name_;  // Options::application_name có NUL cuối cho libpq
    bool in_transaction_ = false;
    std::array<char, kMaxErrorMessage> error_{};
    usize error_length_ = 0;
};

void Connection::Impl::record(std::string_view message) noexcept {
    while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
        message.remove_suffix(1);
    }
    // Thông điệp đến từ server hay libpq, không ai bảo đảm nó là UTF-8 hợp lệ: chỉ bỏ điểm mã bị
    // cắt dở, không kiểm cả chuỗi.
    const std::string_view kept = core::utf8_drop_incomplete_tail(message.substr(0, error_.size()));
    std::ranges::copy(kept, error_.begin());
    error_length_ = kept.size();
}

std::unexpected<Error> Connection::Impl::lost(const ErrorContext context) noexcept {
    if (conn_ != nullptr) {
        record(view(PQerrorMessage(conn_)));
    }
    close();
    return fail(ErrorCode::Unavailable, context);
}

std::unexpected<Error> Connection::Impl::abandon(const Error& error) noexcept {
    close();
    return std::unexpected(error);
}

Result<void> Connection::Impl::connect(const std::string_view conninfo,
                                       const core::MonoTime deadline) {
    close();
    clear_error();
    if (options_.statement_timeout < core::Duration{} ||
        options_.client_check_interval < core::Duration{}) {
        return fail(ErrorCode::InvalidArgument, "db: hạn âm trong Options");
    }
    if (conninfo.find('\0') != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "db: chuỗi kết nối có ký tự NUL");
    }
    if (clock_->now() >= deadline) {
        return fail(ErrorCode::DeadlineExceeded, "db: đã quá hạn trước khi kết nối");
    }
    const std::string info(conninfo);
    if (const Result<void> parsed = check_conninfo(info); !parsed) {
        return parsed;
    }
    // dbname mang cả chuỗi kết nối (expand_dbname = 1); khoá đứng sau đè khoá cùng tên trong chuỗi
    // đó, nên client_encoding luôn là UTF8, thứ value.hpp dựa vào.
    const std::array<const char*, 4> keywords{"dbname", "client_encoding",
                                              "fallback_application_name", nullptr};
    const std::array<const char*, 4> values{info.c_str(), "UTF8", application_name_.c_str(),
                                            nullptr};
    conn_ = PQconnectStartParams(keywords.data(), values.data(), 1);
    if (conn_ == nullptr) {
        return fail(ErrorCode::ResourceExhausted, "db: libpq không cấp phát được kết nối");
    }
    PQsetNoticeReceiver(conn_, &receive_notice, nullptr);
    if (Result<void> polled = poll_connect(deadline); !polled) {
        return polled;
    }
    return prepare_session(deadline);
}

// Kiểm cú pháp trước, để chuỗi sai là InvalidArgument chứ không lẫn với lỗi mạng.
Result<void> Connection::Impl::check_conninfo(const std::string& conninfo) noexcept {
    char* message = nullptr;
    PQconninfoOption* parsed = PQconninfoParse(conninfo.c_str(), &message);
    if (parsed == nullptr) {
        record(view(message));
        PQfreemem(message);
        return fail(ErrorCode::InvalidArgument, "db: chuỗi kết nối sai cú pháp");
    }
    PQconninfoFree(parsed);
    return {};
}

Result<void> Connection::Impl::poll_connect(const core::MonoTime deadline) noexcept {
    if (PQstatus(conn_) == CONNECTION_BAD) {
        return lost("db: không kết nối được tới server");
    }
    // Tài liệu libpq: sau PQconnectStartParams, làm như PQconnectPoll vừa trả
    // PGRES_POLLING_WRITING.
    PostgresPollingStatusType status = PGRES_POLLING_WRITING;
    while (status != PGRES_POLLING_OK) {
        if (status == PGRES_POLLING_FAILED) {
            return lost("db: không kết nối được tới server");
        }
        const detail::Wait what =
            status == PGRES_POLLING_READING ? detail::Wait::Read : detail::Wait::Write;
        if (const Result<void> ready = wait(what, deadline); !ready) {
            return abandon(ready.error());
        }
        status = PQconnectPoll(conn_);
    }
    return {};
}

Result<void> Connection::Impl::prepare_session(const core::MonoTime deadline) noexcept {
    if (PQsetnonblocking(conn_, 1) != 0) {
        return lost("db: không đặt được kết nối không chặn");
    }
    const int version = PQserverVersion(conn_);
    if (version < kMinServerVersion) {
        close();
        return fail(ErrorCode::FailedPrecondition, "db: server cũ hơn PostgreSQL 14", version);
    }
    std::array<char, 16> statement{};
    std::array<char, 16> check{};
    const std::array params{
        Param::text(milliseconds_text(options_.statement_timeout, statement)),
        Param::text(milliseconds_text(options_.client_check_interval, check)),
    };
    const Result<Rows> set = execute(
        "SELECT set_config('statement_timeout', $1, false), "
        "set_config('client_connection_check_interval', $2, false)",
        params, deadline);
    if (!set) {
        close();
        return std::unexpected(set.error());
    }
    return {};
}

Result<void> Connection::Impl::ready_to_send(const core::MonoTime deadline) const noexcept {
    if (conn_ == nullptr) {
        return fail(ErrorCode::Unavailable, "db: chưa kết nối hay kết nối đã đóng");
    }
    // Chưa gửi gì nên kết nối vẫn dùng được.
    if (clock_->now() >= deadline) {
        return fail(ErrorCode::DeadlineExceeded, "db: đã quá hạn trước khi gửi câu");
    }
    return {};
}

Result<void> Connection::Impl::wait(const detail::Wait what,
                                    const core::MonoTime deadline) const noexcept {
    while (true) {
        const core::Duration remaining = deadline - clock_->now();
        if (remaining <= core::Duration{}) {
            return fail(ErrorCode::DeadlineExceeded, "db: quá hạn khi chờ server");
        }
        const int socket = PQsocket(conn_);
        if (socket < 0) {
            return fail(ErrorCode::Unavailable, "db: kết nối không có socket");
        }
        const Result<bool> ready =
            detail::wait_socket(socket, what, detail::poll_timeout_ms(remaining));
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (*ready) {
            return {};
        }
    }
}

// Kết nối không chặn: đẩy dữ liệu gửi tới khi hết, và đọc dữ liệu tới trong lúc đó để server không
// bị kẹt vì client không đọc (tài liệu libpq, "Asynchronous Command Processing").
Result<void> Connection::Impl::flush(const core::MonoTime deadline) noexcept {
    while (true) {
        const int pending = PQflush(conn_);
        if (pending == 0) {
            return {};
        }
        if (pending < 0) {
            return lost("db: gửi câu tới server thất bại");
        }
        if (const Result<void> ready = wait(detail::Wait::ReadWrite, deadline); !ready) {
            return abandon(ready.error());
        }
        if (PQconsumeInput(conn_) == 0) {
            return lost("db: mất kết nối khi gửi câu");
        }
    }
}

// Đọc tới khi libpq có đủ một kết quả, chờ socket có hạn giữa các lần đọc.
Result<void> Connection::Impl::await_result(const core::MonoTime deadline) noexcept {
    while (PQisBusy(conn_) != 0) {
        if (const Result<void> ready = wait(detail::Wait::Read, deadline); !ready) {
            return abandon(ready.error());
        }
        if (PQconsumeInput(conn_) == 0) {
            return lost("db: mất kết nối khi chờ kết quả");
        }
    }
    return {};
}

// Đọc mọi kết quả của lệnh vừa gửi, để kết nối sẵn cho lệnh sau. Kết quả lỗi đầu tiên thắng; không
// có lỗi thì giữ kết quả của câu cuối (script nhiều câu).
Result<ResultPtr> Connection::Impl::receive(const core::MonoTime deadline) noexcept {
    if (const Result<void> flushed = flush(deadline); !flushed) {
        return std::unexpected(flushed.error());
    }
    ResultPtr kept;
    while (true) {
        if (const Result<void> ready = await_result(deadline); !ready) {
            return std::unexpected(ready.error());
        }
        ResultPtr next(PQgetResult(conn_));
        if (next == nullptr) {
            break;
        }
        const ExecStatusType status = PQresultStatus(next.get());
        if (status == PGRES_COPY_IN || status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH) {
            close();
            return fail(ErrorCode::Unimplemented, "db: server_db không hỗ trợ COPY");
        }
        if (kept == nullptr || PQresultStatus(kept.get()) != PGRES_FATAL_ERROR) {
            kept = std::move(next);
        }
    }
    if (kept == nullptr) {
        return fail(ErrorCode::Internal, "db: server không trả kết quả nào");
    }
    return kept;
}

Result<Rows> Connection::Impl::collect(const core::MonoTime deadline) noexcept {
    Result<ResultPtr> result = receive(deadline);
    if (!result) {
        return std::unexpected(result.error());
    }
    return to_rows(std::move(*result));
}

Result<Rows> Connection::Impl::to_rows(ResultPtr result) noexcept {
    const ExecStatusType status = PQresultStatus(result.get());
    switch (status) {
        case PGRES_COMMAND_OK:
        case PGRES_TUPLES_OK:
        case PGRES_EMPTY_QUERY:
            return Rows(result.release());
        case PGRES_FATAL_ERROR:
            return server_error(*result);
        default:
            record(view(PQresultErrorMessage(result.get())));
            close();
            return fail(ErrorCode::Internal, "db: server trả trạng thái kết quả lạ",
                        static_cast<i64>(status));
    }
}

std::unexpected<Error> Connection::Impl::server_error(const PGresult& result) noexcept {
    const char* primary = PQresultErrorField(&result, PG_DIAG_MESSAGE_PRIMARY);
    record(primary != nullptr ? view(primary) : view(PQresultErrorMessage(&result)));
    const std::string_view severity =
        view(PQresultErrorField(&result, PG_DIAG_SEVERITY_NONLOCALIZED));
    // FATAL và PANIC: server kết thúc phiên ngay sau lỗi này.
    if (severity == "FATAL" || severity == "PANIC" || PQstatus(conn_) != CONNECTION_OK) {
        close();
    }
    const char* state = PQresultErrorField(&result, PG_DIAG_SQLSTATE);
    if (state != nullptr) {
        return std::unexpected(detail::sqlstate_error(state));
    }
    // Lỗi do chính libpq tạo, ví dụ mất kết nối giữa chừng, không có SQLSTATE.
    if (conn_ == nullptr) {
        return fail(ErrorCode::Unavailable, "db: mất kết nối tới server");
    }
    return fail(ErrorCode::Internal, "db: libpq báo lỗi không có SQLSTATE");
}

Result<Rows> Connection::Impl::execute(const Sql sql, const std::span<const Param> params,
                                       const core::MonoTime deadline) noexcept {
    ORION_ASSERT(params.size() <= kMaxParams, "db: {} tham số, tối đa {}", params.size(),
                 kMaxParams);
    clear_error();
    if (const Result<void> ready = ready_to_send(deadline); !ready) {
        return std::unexpected(ready.error());
    }
    std::array<Oid, kMaxParams> types{};
    std::array<const char*, kMaxParams> values{};
    std::array<int, kMaxParams> lengths{};
    std::array<int, kMaxParams> formats{};
    for (usize i = 0; i < params.size(); ++i) {
        const std::span<const std::byte> wire = params[i].wire();
        if (wire.size() > kMaxParamBytes) {
            return fail(ErrorCode::OutOfRange, "db: tham số lớn hơn 1 GiB", static_cast<i64>(i));
        }
        types[i] = params[i].type();
        // Con trỏ null là NULL với libpq, nên giá trị rỗng mà không NULL phải trỏ vào đâu đó.
        if (!params[i].is_null()) {
            values[i] = wire.empty() ? "" : core::as_chars(wire).data();
        }
        lengths[i] = static_cast<int>(wire.size());
        formats[i] = 1;  // định dạng nhị phân
    }
    if (PQsendQueryParams(conn_, sql.c_str(), static_cast<int>(params.size()), types.data(),
                          values.data(), lengths.data(), formats.data(), 1) == 0) {
        return lost("db: không gửi được câu");
    }
    return collect(deadline);
}

Result<void> Connection::Impl::script(const std::string_view text, const core::MonoTime deadline) {
    clear_error();
    if (text.find('\0') != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "db: script có ký tự NUL");
    }
    if (const Result<void> ready = ready_to_send(deadline); !ready) {
        return ready;
    }
    const std::string sql(text);
    if (PQsendQuery(conn_, sql.c_str()) == 0) {
        return lost("db: không gửi được script");
    }
    const Result<Rows> rows = collect(deadline);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return {};
}

Rows::Rows(Rows&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}

Rows& Rows::operator=(Rows&& other) noexcept {
    if (this != &other) {
        PQclear(native(result_));
        result_ = std::exchange(other.result_, nullptr);
    }
    return *this;
}

Rows::~Rows() {
    PQclear(native(result_));
}

usize Rows::size() const noexcept {
    return static_cast<usize>(PQntuples(native(result_)));
}

usize Rows::columns() const noexcept {
    return static_cast<usize>(PQnfields(native(result_)));
}

u64 Rows::affected() const noexcept {
    const std::string_view digits = view(PQcmdTuples(native(result_)));
    u64 count = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), count);
    // libpq trả chuỗi rỗng hay toàn chữ số; số tràn u64 không có với server thật (PostgreSQL đếm
    // dòng bằng uint64).
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return 0;
    }
    return count;
}

Result<bool> Rows::is_null(const usize row, const usize column) const noexcept {
    const Result<Cell> cell = cell_at(native(result_), row, column);
    if (!cell) {
        return std::unexpected(cell.error());
    }
    return cell->null;
}

Result<bool> Rows::boolean(const usize row, const usize column) const noexcept {
    return read_cell<bool>(result_, row, column, &decode_boolean);
}

Result<i64> Rows::integer(const usize row, const usize column) const noexcept {
    return read_cell<i64>(result_, row, column, &decode_integer);
}

Result<f64> Rows::real(const usize row, const usize column) const noexcept {
    return read_cell<f64>(result_, row, column, &decode_real);
}

Result<std::string_view> Rows::text(const usize row, const usize column) const noexcept {
    return read_cell<std::string_view>(result_, row, column, &decode_text);
}

Result<std::span<const std::byte>> Rows::bytes(const usize row, const usize column) const noexcept {
    return read_cell<std::span<const std::byte>>(result_, row, column, &decode_bytes);
}

Result<core::WallTime> Rows::time(const usize row, const usize column) const noexcept {
    return read_cell<core::WallTime>(result_, row, column, &decode_time);
}

Connection::Connection(const core::MonotonicClock& clock, const Options& options)
    : impl_(std::make_unique<Impl>(clock, options)) {}

Connection::Connection(Connection&& other) noexcept = default;

Connection& Connection::operator=(Connection&& other) noexcept {
    // Transaction trỏ vào Impl của kết nối bị thay; huỷ nó khi transaction còn mở là dùng vùng nhớ
    // đã giải phóng.
    ORION_VERIFY(impl_ == nullptr || impl_.get() == other.impl_.get() || !impl_->in_transaction(),
                 "db: kết nối bị thay khi còn transaction mở");
    impl_ = std::move(other.impl_);
    return *this;
}

Connection::~Connection() {
    ORION_VERIFY(impl_ == nullptr || !impl_->in_transaction(),
                 "db: kết nối bị huỷ khi còn transaction mở");
}

Connection::Impl& Connection::impl() const noexcept {
    ORION_VERIFY(impl_ != nullptr, "db: dùng Connection đã bị chuyển đi");
    return *impl_;
}

Result<void> Connection::connect(const std::string_view conninfo, const core::MonoTime deadline) {
    Impl& state = impl();
    // Kết nối lại khi transaction còn mở thì COMMIT của nó sẽ chạy trên một phiên khác.
    ORION_VERIFY(!state.in_transaction(), "db: kết nối lại khi còn transaction mở");
    return state.connect(conninfo, deadline);
}

bool Connection::connected() const noexcept {
    return impl().connected();
}

void Connection::close() noexcept {
    impl().close();
}

std::string_view Connection::last_error() const noexcept {
    return impl().last_error();
}

Result<Rows> Connection::execute(const Sql sql, const std::span<const Param> params,
                                 const core::MonoTime deadline) noexcept {
    Impl& state = impl();
    ORION_ASSERT(!state.in_transaction(), "db: trong transaction thì chạy câu qua Transaction");
    return state.execute(sql, params, deadline);
}

Result<void> Connection::execute_script(const std::string_view script,
                                        const core::MonoTime deadline) {
    Impl& state = impl();
    ORION_ASSERT(!state.in_transaction(), "db: trong transaction thì chạy script qua Transaction");
    return state.script(script, deadline);
}

Result<Transaction> Connection::begin(const Isolation isolation,
                                      const core::MonoTime deadline) noexcept {
    Impl& state = impl();
    ORION_ASSERT(!state.in_transaction(), "db: kết nối đã có transaction mở");
    if (const Result<Rows> begun = state.execute(begin_sql(isolation), {}, deadline); !begun) {
        return std::unexpected(begun.error());
    }
    state.set_in_transaction(true);
    return Transaction(state, deadline);
}

Transaction::Transaction(Connection::Impl& impl, const core::MonoTime deadline) noexcept
    : impl_(&impl), deadline_(deadline) {}

Transaction::Transaction(Transaction&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)), deadline_(other.deadline_) {}

Transaction::~Transaction() {
    if (impl_ == nullptr) {
        return;
    }
    Connection::Impl& impl = finish();
    if (!impl.connected()) {
        return;
    }
    const Result<Rows> rolled_back =
        impl.execute("ROLLBACK", {}, impl.clock().now() + kRollbackTimeout);
    // Không ROLLBACK được thì đóng kết nối: server tự ROLLBACK khi thấy kết nối đóng.
    if (!rolled_back) {
        impl.close();
    }
    impl.leave_clean();
}

Connection::Impl& Transaction::finish() noexcept {
    ORION_VERIFY(impl_ != nullptr, "db: transaction đã kết thúc");
    Connection::Impl& impl = *std::exchange(impl_, nullptr);
    impl.set_in_transaction(false);
    return impl;
}

Result<Rows> Transaction::execute(const Sql sql, const std::span<const Param> params) noexcept {
    ORION_VERIFY(impl_ != nullptr, "db: transaction đã kết thúc");
    return impl_->execute(sql, params, deadline_);
}

Result<void> Transaction::execute_script(const std::string_view script) {
    ORION_VERIFY(impl_ != nullptr, "db: transaction đã kết thúc");
    return impl_->script(script, deadline_);
}

Result<void> Transaction::commit() noexcept {
    Connection::Impl& impl = finish();
    const Result<Rows> committed = impl.execute("COMMIT", {}, deadline_);
    if (!committed) {
        // Câu COMMIT không tới được server (ví dụ đã quá hạn trước khi gửi): transaction còn mở ở
        // server, nên kết nối phải đóng.
        impl.leave_clean();
        return std::unexpected(committed.error());
    }
    // COMMIT của một transaction đã hỏng được server làm thành ROLLBACK mà không báo lỗi; thẻ lệnh
    // trả về là "ROLLBACK".
    if (view(PQcmdStatus(native(committed->result_))) != "COMMIT") {
        impl.record("COMMIT thành ROLLBACK vì transaction đã hỏng");
        return fail(ErrorCode::Aborted, "db: transaction đã hỏng, COMMIT thành ROLLBACK");
    }
    return {};
}

Result<void> Transaction::rollback() noexcept {
    Connection::Impl& impl = finish();
    const Result<Rows> rolled_back = impl.execute("ROLLBACK", {}, deadline_);
    if (!rolled_back) {
        impl.close();
        return std::unexpected(rolled_back.error());
    }
    return {};
}

}  // namespace orion::db
