#pragma once

// Bọc libpq (ARCH §4.5, §4.6; CLAUDE.md X.14; ADR 0013): kết nối PostgreSQL, truy vấn có tham số,
// transaction.
//
// - Câu SQL là kiểu Sql, chỉ dựng được lúc biên dịch từ literal chuỗi: giá trị không bao giờ bị
// ghép
//   vào SQL mà luôn đi qua tham số (Param, value.hpp), nên không cần hàm escape nào của libpq.
//   Ngoại lệ duy nhất là execute_script, cho văn bản SQL tin cậy của repo: migration và test.
// - Mọi lời gọi có hạn (X.14): mỗi lời gọi nhận một mốc hạn trên đồng hồ đơn điệu. Code dùng API
//   bất đồng bộ của libpq và chờ socket không quá mốc đó. Quá hạn khi truy vấn đã gửi thì kết nối
//   bị đóng, vì huỷ truy vấn cần mở thêm một kết nối, việc cũng có thể treo; server thấy client mất
//   thì tự huỷ truy vấn (Options::client_check_interval), và statement_timeout của server chặn
//   thêm.
// - Tham số và kết quả đi ở định dạng nhị phân (value.hpp).
// - Lỗi của server thành Error theo SQLSTATE (bảng ở detail/sqlstate.cpp); detail() là SQLSTATE gói
//   vào một số (sqlstate_code), để bên gọi phân biệt, ví dụ trùng khoá (kUniqueViolation). Thông
//   điệp của lỗi đọc ở last_error(), để nơi xử lý lỗi log đúng một lần (X.5).
// - Một Connection chỉ được một luồng dùng tại một thời điểm. Header không lộ kiểu nào của libpq.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/value.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace orion::db {

namespace detail {
// Không định nghĩa ở đâu cả: Sql gọi nó khi literal có NUL ở giữa, và vì hàm không constexpr nên
// lời gọi đó thành lỗi biên dịch.
void sql_literal_has_nul() noexcept;
}  // namespace detail

// Một câu SQL: literal chuỗi, sống suốt chương trình, kết thúc bằng NUL.
class Sql {
public:
    // Nội dung được đọc lúc biên dịch, nên chỉ literal hay mảng constexpr qua được; mảng ghi được
    // lúc chạy thì không.
    template <usize N>
    // NOLINTNEXTLINE(*-avoid-c-arrays,*-pro-bounds-array-to-pointer-decay): chỉ nhận literal chuỗi.
    consteval Sql(const char (&text)[N]) noexcept : text_(text) {
        for (usize i = 0; i + 1 < N; ++i) {
            if (text[i] == '\0') {
                detail::sql_literal_has_nul();
            }
        }
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept { return text_; }

private:
    const char* text_;
};

// SQLSTATE năm ký tự gói vào một số: detail() của lỗi từ server, để so với các hằng dưới đây.
[[nodiscard]] constexpr i64 sqlstate_code(const std::string_view state) noexcept {
    i64 code = 0;
    for (const char c : state.substr(0, 5)) {
        code = (code << 8U) | static_cast<unsigned char>(c);
    }
    return code;
}

inline constexpr i64 kUniqueViolation = sqlstate_code("23505");
inline constexpr i64 kForeignKeyViolation = sqlstate_code("23503");
inline constexpr i64 kCheckViolation = sqlstate_code("23514");
inline constexpr i64 kSerializationFailure = sqlstate_code("40001");
inline constexpr i64 kDeadlockDetected = sqlstate_code("40P01");

// Ngược lại sqlstate_code, cho log: "23505".
[[nodiscard]] std::array<char, 5> sqlstate_text(i64 code) noexcept;

// Số tham số tối đa của một truy vấn: các mảng đưa cho libpq nằm trên stack.
inline constexpr usize kMaxParams = 32;

// Transaction bị huỷ khi chưa commit thì ROLLBACK với hạn này, tính từ lúc huỷ: hạn của request có
// thể đã qua, mà trả kết nối sạch về pool vẫn đáng. Không kịp thì kết nối bị đóng.
inline constexpr core::Duration kRollbackTimeout = core::Duration::seconds(1);

struct Options {
    // statement_timeout của server: câu nào chạy quá thì server tự huỷ (DeadlineExceeded). 0 là
    // không giới hạn. Đây là lớp chặn thứ hai, sau hạn của từng lời gọi.
    core::Duration statement_timeout = core::Duration::seconds(30);
    // client_connection_check_interval của server (PostgreSQL 14 trở lên, server chạy Linux): khi
    // kết nối bị đóng vì quá hạn, server phát hiện trong khoảng này và huỷ truy vấn còn chạy. 0 là
    // tắt.
    core::Duration client_check_interval = core::Duration::seconds(1);
    // application_name hiện trong pg_stat_activity, khi chuỗi kết nối không tự đặt.
    std::string_view application_name = "orion";
};

enum class Isolation : u8 {
    ReadCommitted,
    RepeatableRead,
    Serializable,
};

// Kết quả của một câu: bảng các ô, và số dòng bị đổi. Sống độc lập với Connection.
class Rows {
public:
    Rows(Rows&& other) noexcept;
    Rows& operator=(Rows&& other) noexcept;
    Rows(const Rows&) = delete;
    Rows& operator=(const Rows&) = delete;
    ~Rows();

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] usize columns() const noexcept;
    // Số dòng INSERT, UPDATE, DELETE, MERGE đã đổi (SELECT: đã trả); 0 với câu khác.
    [[nodiscard]] u64 affected() const noexcept;

    // Lỗi: OutOfRange khi ô ngoài bảng; FailedPrecondition khi ô là NULL (trừ is_null); lỗi của
    // decode_* (value.hpp) khi cột có kiểu khác hay giá trị hỏng.
    [[nodiscard]] Result<bool> is_null(usize row, usize column) const noexcept;
    [[nodiscard]] Result<bool> boolean(usize row, usize column) const noexcept;
    [[nodiscard]] Result<i64> integer(usize row, usize column) const noexcept;
    [[nodiscard]] Result<f64> real(usize row, usize column) const noexcept;
    // View và span sống tới khi Rows bị huỷ.
    [[nodiscard]] Result<std::string_view> text(usize row, usize column) const noexcept;
    [[nodiscard]] Result<std::span<const std::byte>> bytes(usize row, usize column) const noexcept;
    [[nodiscard]] Result<core::WallTime> time(usize row, usize column) const noexcept;

private:
    friend class Connection;
    friend class Transaction;
    explicit Rows(void* result) noexcept : result_(result) {}

    // PGresult* của libpq, giấu kiểu để header không lộ libpq (ADR 0013).
    void* result_ = nullptr;
};

class Transaction;

class Connection {
public:
    // Chưa kết nối; connect() mở kết nối. `clock` đo hạn của mọi lời gọi và phải sống lâu hơn
    // Connection.
    explicit Connection(const core::MonotonicClock& clock, const Options& options = {});

    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    // Kết nối, hay kết nối lại (kết nối cũ, nếu có, bị đóng trước), theo `conninfo`: chuỗi kết nối
    // của libpq dạng key=value hay URI. Phiên luôn dùng client_encoding UTF8 và các hạn của
    // Options. Tên máy được tra DNS đồng bộ bên trong libpq, ngoài hạn: dùng hostaddr để tránh.
    // Lỗi: InvalidArgument (chuỗi sai cú pháp hay có NUL, hạn âm trong Options), Unavailable (không
    // kết nối được; last_error() nói vì sao), DeadlineExceeded, FailedPrecondition (server cũ hơn
    // PostgreSQL 14), và lỗi của server khi đặt hạn.
    [[nodiscard]] Result<void> connect(std::string_view conninfo, core::MonoTime deadline);

    [[nodiscard]] bool connected() const noexcept;
    void close() noexcept;
    // Thông điệp của lỗi gần nhất: thông điệp chính của server (không kèm DETAIL, phần hay chứa giá
    // trị của dòng), hay của libpq; đã cắt còn tối đa 255 byte. Rỗng khi lời gọi gần nhất thành
    // công. Sống tới lời gọi sau.
    [[nodiscard]] std::string_view last_error() const noexcept;

    // Một câu SQL với tối đa kMaxParams tham số, đánh số $1, $2, ... theo thứ tự. Lỗi: Unavailable
    // (chưa kết nối, mất kết nối), DeadlineExceeded, OutOfRange (tham số lớn hơn 1 GiB), và lỗi của
    // server theo SQLSTATE.
    [[nodiscard]] Result<Rows> execute(Sql sql, std::span<const Param> params,
                                       core::MonoTime deadline) noexcept;
    // Nhiều câu SQL trong một lần gửi, không tham số, chạy như một transaction ngầm: câu lỗi đầu
    // tiên huỷ cả script. Chỉ cho văn bản tin cậy của repo (migration, test), không bao giờ cho
    // chuỗi dựng từ dữ liệu.
    [[nodiscard]] Result<void> execute_script(std::string_view script, core::MonoTime deadline);

    // Mở transaction. Trong lúc nó mở, mọi câu đi qua Transaction, và Connection không được kết nối
    // lại, chuyển đi hay huỷ.
    [[nodiscard]] Result<Transaction> begin(Isolation isolation, core::MonoTime deadline) noexcept;

private:
    friend class Transaction;
    class Impl;

    [[nodiscard]] Impl& impl() const noexcept;

    std::unique_ptr<Impl> impl_;
};

// Transaction trên một Connection. Mọi câu trong nó dùng mốc hạn đã đưa cho begin. Bị huỷ khi chưa
// commit hay rollback thì ROLLBACK (kRollbackTimeout); không được thì kết nối bị đóng, và server tự
// ROLLBACK khi thấy kết nối đóng.
class Transaction {
public:
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    [[nodiscard]] Result<Rows> execute(Sql sql, std::span<const Param> params = {}) noexcept;
    [[nodiscard]] Result<void> execute_script(std::string_view script);
    // Lỗi Aborted khi PostgreSQL đã huỷ transaction (xung đột serializable, deadlock, hay một câu
    // trước đó lỗi): bên gọi chạy lại cả transaction. Lỗi Unavailable hay DeadlineExceeded lúc
    // commit: không biết transaction đã commit hay chưa, nên mọi ghi bền phải idempotent (ledger
    // dùng khoá idempotency). Sau commit, dù thành công hay lỗi, transaction đã kết thúc.
    [[nodiscard]] Result<void> commit() noexcept;
    // Lỗi thì kết nối đã bị đóng, và server tự ROLLBACK.
    [[nodiscard]] Result<void> rollback() noexcept;

private:
    friend class Connection;
    Transaction(Connection::Impl& impl, core::MonoTime deadline) noexcept;
    // Kết thúc transaction trên kết nối và trả kết nối đó; gọi lần hai là lỗi lập trình.
    [[nodiscard]] Connection::Impl& finish() noexcept;

    Connection::Impl* impl_;
    core::MonoTime deadline_;
};

}  // namespace orion::db
