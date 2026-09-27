#pragma once

// Log bất đồng bộ (ARCH §4.1, CLAUDE.md X.5, ADR 0007).
//
// Luồng gọi định dạng thông điệp bằng std::format vào một LogRecord cỡ cố định (không cấp phát),
// rồi đẩy nó vào một MpmcRing. Luồng ghi log (một luồng mỗi tiến trình, ADR 0007) gọi run() để lấy
// bản ghi và ghi ra LogSink: TextSink cho người đọc ở DEV, JsonLinesSink cho server.
//
// Luật (X.5): mức trace, debug, info, warn, error; hot path không log trừ khi qua LogRateLimiter;
// không log bí mật (mật khẩu, token, khoá); dữ liệu cá nhân chỉ log dạng đã băm. Hàng đợi đầy thì
// bản ghi bị bỏ và đếm, không bao giờ chặn luồng gọi.

#include "engine/core/queue.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace orion::core {

// Giá trị tường minh vì mức log được so sánh và lưu trong cấu hình (X.3).
enum class LogLevel : u8 {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

// "trace", "debug", "info", "warn", "error".
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

// Trường có cấu trúc của server (X.5): zone, tick, entity khi có.
// Trường nào không có thì để trống; khởi tạo mặc định tường minh để designated initializer chỉ cần
// ghi trường có mặt, ví dụ `{.zone = 3, .tick = t}`.
struct LogFields {
    std::optional<u32> zone = std::nullopt;
    std::optional<u64> tick = std::nullopt;
    std::optional<u64> entity = std::nullopt;
};

// Một bản ghi đã định dạng xong. Cỡ cố định để đi qua hàng đợi mà không cấp phát.
struct LogRecord {
    static constexpr usize kMaxMessage = 440;

    WallTime time;
    LogFields fields;
    LogLevel level = LogLevel::Info;
    bool truncated = false;  // thông điệp dài hơn kMaxMessage và đã bị cắt ở biên điểm mã
    u16 length = 0;
    std::array<char, kMaxMessage> message{};

    [[nodiscard]] std::string_view text() const noexcept { return {message.data(), length}; }
};

// Định dạng một bản ghi thành một dòng (có '\n' cuối) vào `out`; trả phần đã ghi. `out` phải đủ
// kLineCapacity byte, đủ cho trường hợp xấu nhất khi mọi byte của thông điệp phải escape.
inline constexpr usize kLineCapacity = 4096;
[[nodiscard]] std::string_view format_text_line(const LogRecord& record,
                                                std::span<char, kLineCapacity> out) noexcept;
// JSON lines (X.5): {"ts":"…","level":"info","zone":1,"tick":5,"entity":7,"msg":"…"}. Byte UTF-8
// hỏng trong thông điệp được thay bằng U+FFFD để mỗi dòng luôn là JSON hợp lệ.
[[nodiscard]] std::string_view format_json_line(const LogRecord& record,
                                                std::span<char, kLineCapacity> out) noexcept;

// Đích ghi của luồng ghi log. Chỉ luồng ghi log gọi write và flush.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) noexcept = 0;
    virtual void flush() noexcept = 0;

protected:
    LogSink() = default;
    LogSink(const LogSink&) = default;
    LogSink(LogSink&&) = default;
    LogSink& operator=(const LogSink&) = default;
    LogSink& operator=(LogSink&&) = default;
};

// Ghi ra một FILE* do người gọi sở hữu và giữ mở lâu hơn sink.
class TextSink final : public LogSink {
public:
    explicit TextSink(std::FILE* stream) noexcept : stream_(stream) {}
    void write(const LogRecord& record) noexcept override;
    void flush() noexcept override;

private:
    std::FILE* stream_;
};

class JsonLinesSink final : public LogSink {
public:
    explicit JsonLinesSink(std::FILE* stream) noexcept : stream_(stream) {}
    void write(const LogRecord& record) noexcept override;
    void flush() noexcept override;

private:
    std::FILE* stream_;
};

class Logger {
public:
    struct Config {
        LogLevel min_level = LogLevel::Info;
        u32 capacity = 4096;  // số bản ghi chờ tối đa; làm tròn lên lũy thừa của 2
    };

    // `clock` phải sống lâu hơn logger. Cấp phát hàng đợi ở đây, một lần.
    Logger(const WallClock& clock, Config config);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;
    ~Logger() = default;

    // Gọi được từ mọi luồng.
    [[nodiscard]] bool enabled(const LogLevel level) const noexcept {
        // relaxed: mức log là cấu hình độc lập, không đi kèm dữ liệu nào khác.
        return std::to_underlying(level) >= min_level_.load(std::memory_order_relaxed);
    }
    void set_min_level(const LogLevel level) noexcept {
        min_level_.store(std::to_underlying(level), std::memory_order_relaxed);
    }

    // Định dạng và xếp một bản ghi. Gọi được từ mọi luồng; không cấp phát, không chặn.
    template <class... Args>
    void write(const LogLevel level, const LogFields& fields,
               const std::format_string<Args...> format, Args&&... args) noexcept {
        if (!enabled(level)) {
            return;
        }
        LogRecord record;
        record.time = clock_.now();
        record.level = level;
        record.fields = fields;
        const auto result =
            std::format_to_n(record.message.data(), static_cast<isize>(record.message.size()),
                             format, std::forward<Args>(args)...);
        const auto written = std::min(static_cast<usize>(result.size), record.message.size());
        const bool truncated = static_cast<usize>(result.size) > written;
        std::string_view kept(record.message.data(), written);
        if (truncated) {
            kept = utf8_drop_incomplete_tail(kept);
        }
        record.length = static_cast<u16>(kept.size());
        record.truncated = truncated;
        submit(record);
    }

    // Luồng ghi log gọi: lấy mọi bản ghi đang chờ và ghi ra sink. Trả số bản ghi đã ghi.
    usize drain(LogSink& sink) noexcept;
    // Vòng lặp của luồng ghi log: drain, rồi chờ bản ghi mới, cho tới khi stop() được gọi; trước
    // khi trả về thì ghi nốt mọi bản ghi còn lại và flush.
    void run(LogSink& sink) noexcept;
    // Gọi được từ mọi luồng; run() trả về sau khi ghi nốt.
    void stop() noexcept;

    // Số bản ghi bị bỏ vì hàng đợi đầy, từ khi dựng.
    [[nodiscard]] u64 dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    void submit(const LogRecord& record) noexcept;

    const WallClock& clock_;
    std::atomic<u8> min_level_;
    MpmcRing<LogRecord> queue_;
    std::atomic<u64> dropped_{0};
    u64 dropped_reported_ = 0;  // chỉ luồng ghi log đọc và ghi
    std::atomic<u64> pending_{0};
    std::atomic<bool> stopping_{false};
};

// Giới hạn tần suất log trên hot path (X.5): cho qua tối đa `burst` lần mỗi `interval`. Một đối
// tượng cho mỗi chỗ gọi; không đồng bộ, thuộc một luồng.
class LogRateLimiter {
public:
    LogRateLimiter(const MonotonicClock& clock, Duration interval, u32 burst) noexcept;

    // true nếu lần này được log.
    [[nodiscard]] bool allow() noexcept;
    // Số lần bị chặn từ lần gọi trước của hàm này; dùng để log "đã bỏ N lần" khi được phép lại.
    [[nodiscard]] u64 take_suppressed() noexcept { return std::exchange(suppressed_, 0); }

private:
    const MonotonicClock* clock_;  // không null, sống lâu hơn đối tượng này
    Duration interval_;
    MonoTime window_start_;
    u32 burst_;
    u32 used_ = 0;
    u64 suppressed_ = 0;
};

// Logger toàn cục (CLAUDE.md X.3: ngoại lệ duy nhất cho trạng thái toàn cục cùng allocator gốc).
// Gọi set_global_logger đúng một lần trong main, trước khi tạo luồng nào khác, và gọi lại với
// nullptr trước khi logger bị huỷ.
void set_global_logger(Logger* logger) noexcept;
[[nodiscard]] Logger* global_logger() noexcept;

template <class... Args>
void log(const LogLevel level, const LogFields& fields, const std::format_string<Args...> format,
         Args&&... args) noexcept {
    if (Logger* logger = global_logger(); logger != nullptr) {
        logger->write(level, fields, format, std::forward<Args>(args)...);
    }
}

template <class... Args>
void log_trace(const std::format_string<Args...> format, Args&&... args) noexcept {
    log(LogLevel::Trace, {}, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_debug(const std::format_string<Args...> format, Args&&... args) noexcept {
    log(LogLevel::Debug, {}, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_info(const std::format_string<Args...> format, Args&&... args) noexcept {
    log(LogLevel::Info, {}, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_warn(const std::format_string<Args...> format, Args&&... args) noexcept {
    log(LogLevel::Warn, {}, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_error(const std::format_string<Args...> format, Args&&... args) noexcept {
    log(LogLevel::Error, {}, format, std::forward<Args>(args)...);
}

}  // namespace orion::core
