#include "engine/core/log.hpp"

#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace orion::core {
namespace {

// Ghi nối vào một bộ đệm cố định, không bao giờ vượt biên: phần không vừa bị bỏ. Bộ đệm đã được
// định cỡ cho trường hợp xấu nhất (kLineCapacity), nên trên thực tế không có gì bị bỏ.
class LineBuilder {
public:
    explicit LineBuilder(const std::span<char, kLineCapacity> out) noexcept : out_(out) {}

    void append(const std::string_view text) noexcept {
        const usize count = std::min(text.size(), out_.size() - size_);
        size_ += text.copy(out_.data() + size_, count);
    }

    void append(const char c) noexcept {
        if (size_ < out_.size()) {
            out_[size_++] = c;
        }
    }

    template <class... Args>
    void format(const std::format_string<Args...> format, Args&&... args) noexcept {
        const auto result =
            std::format_to_n(out_.data() + size_, static_cast<isize>(out_.size() - size_), format,
                             std::forward<Args>(args)...);
        size_ += std::min(static_cast<usize>(result.size), out_.size() - size_);
    }

    [[nodiscard]] std::string_view view() const noexcept { return {out_.data(), size_}; }

private:
    std::span<char, kLineCapacity> out_;
    usize size_ = 0;
};

// Escape theo RFC 8259; byte UTF-8 hỏng thành U+FFFD để dòng luôn là JSON hợp lệ.
void append_json_string(LineBuilder& line, const std::string_view text) noexcept {
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
    line.append('"');
    usize i = 0;
    while (i < text.size()) {
        const usize length = utf8_sequence_length(text, i);
        if (length == 0) {
            line.append(kReplacement);
            ++i;
            continue;
        }
        if (length > 1) {
            line.append(text.substr(i, length));
            i += length;
            continue;
        }
        const char c = text[i++];
        switch (c) {
            case '"':
                line.append("\\\"");
                break;
            case '\\':
                line.append("\\\\");
                break;
            case '\n':
                line.append("\\n");
                break;
            case '\r':
                line.append("\\r");
                break;
            case '\t':
                line.append("\\t");
                break;
            default:
                if (static_cast<u8>(c) < 0x20U) {
                    line.format("\\u{:04x}", static_cast<u32>(static_cast<u8>(c)));
                } else {
                    line.append(c);
                }
        }
    }
    line.append('"');
}

void write_line(std::FILE* stream, const std::string_view line) noexcept {
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), stream));
}

// NOLINTNEXTLINE(*-avoid-non-const-global-variables): logger toàn cục, ngoại lệ của CLAUDE.md X.3.
std::atomic<Logger*> g_global_logger{nullptr};

}  // namespace

std::string_view to_string(const LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "unknown";
}

std::string_view format_text_line(const LogRecord& record,
                                  const std::span<char, kLineCapacity> out) noexcept {
    LineBuilder line{out};
    line.format("{} {:<5}", record.time, to_string(record.level));
    const LogFields& fields = record.fields;
    if (fields.zone || fields.tick || fields.entity) {
        line.append(" [");
        const char* separator = "";
        if (fields.zone) {
            line.format("zone={}", *fields.zone);
            separator = " ";
        }
        if (fields.tick) {
            line.format("{}tick={}", separator, *fields.tick);
            separator = " ";
        }
        if (fields.entity) {
            line.format("{}entity={}", separator, *fields.entity);
        }
        line.append(']');
    }
    line.append(' ');
    line.append(record.text());
    if (record.truncated) {
        line.append(" [đã cắt]");
    }
    line.append('\n');
    return line.view();
}

std::string_view format_json_line(const LogRecord& record,
                                  const std::span<char, kLineCapacity> out) noexcept {
    LineBuilder line{out};
    line.format(R"({{"ts":"{}","level":"{}")", record.time, to_string(record.level));
    if (record.fields.zone) {
        line.format(R"(,"zone":{})", *record.fields.zone);
    }
    if (record.fields.tick) {
        line.format(R"(,"tick":{})", *record.fields.tick);
    }
    if (record.fields.entity) {
        line.format(R"(,"entity":{})", *record.fields.entity);
    }
    line.append(R"(,"msg":)");
    append_json_string(line, record.text());
    if (record.truncated) {
        line.append(R"(,"truncated":true)");
    }
    line.append("}\n");
    return line.view();
}

void TextSink::write(const LogRecord& record) noexcept {
    std::array<char, kLineCapacity> buffer{};
    write_line(stream_, format_text_line(record, buffer));
}

void TextSink::flush() noexcept {
    static_cast<void>(std::fflush(stream_));
}

void JsonLinesSink::write(const LogRecord& record) noexcept {
    std::array<char, kLineCapacity> buffer{};
    write_line(stream_, format_json_line(record, buffer));
}

void JsonLinesSink::flush() noexcept {
    static_cast<void>(std::fflush(stream_));
}

Logger::Logger(const WallClock& clock, const Config config)
    : clock_(clock), min_level_(std::to_underlying(config.min_level)), queue_(config.capacity) {}

void Logger::submit(const LogRecord& record) noexcept {
    if (!queue_.try_push(record)) {
        // relaxed: bộ đếm độc lập, chỉ để báo cáo.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // release: luồng ghi log thấy số mới thì cũng thấy bản ghi đã nằm trong hàng đợi.
    pending_.fetch_add(1, std::memory_order_release);
    pending_.notify_one();
}

usize Logger::drain(LogSink& sink) noexcept {
    usize count = 0;
    LogRecord record;
    while (queue_.try_pop(record)) {
        sink.write(record);
        ++count;
    }
    const u64 dropped_now = dropped_.load(std::memory_order_relaxed);
    if (dropped_now != dropped_reported_) {
        LogRecord notice;
        notice.time = clock_.now();
        notice.level = LogLevel::Warn;
        const auto result = std::format_to_n(
            notice.message.data(), static_cast<isize>(notice.message.size()),
            "log: đã bỏ {} bản ghi vì hàng đợi đầy", dropped_now - dropped_reported_);
        notice.length =
            static_cast<u16>(std::min(static_cast<usize>(result.size), notice.message.size()));
        sink.write(notice);
        dropped_reported_ = dropped_now;
        ++count;
    }
    if (count > 0) {
        sink.flush();
    }
    return count;
}

void Logger::run(LogSink& sink) noexcept {
    while (true) {
        // Lấy số đếm trước khi drain: bản ghi nào đến sau lần đọc này làm wait() trả về ngay, nên
        // không bao giờ lỡ tín hiệu.
        const u64 seen = pending_.load(std::memory_order_acquire);
        static_cast<void>(drain(sink));
        if (stopping_.load(std::memory_order_acquire)) {
            static_cast<void>(drain(sink));
            sink.flush();
            return;
        }
        pending_.wait(seen, std::memory_order_acquire);
    }
}

void Logger::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    // release: ghép với acquire trong run() để luồng ghi log thấy stopping_ sau khi thức dậy.
    pending_.fetch_add(1, std::memory_order_release);
    pending_.notify_one();
}

LogRateLimiter::LogRateLimiter(const MonotonicClock& clock, const Duration interval,
                               const u32 burst) noexcept
    : clock_(&clock), interval_(interval), window_start_(clock.now()), burst_(burst) {}

bool LogRateLimiter::allow() noexcept {
    const MonoTime now = clock_->now();
    if (now - window_start_ >= interval_) {
        window_start_ = now;
        used_ = 0;
    }
    if (used_ < burst_) {
        ++used_;
        return true;
    }
    ++suppressed_;
    return false;
}

void set_global_logger(Logger* logger) noexcept {
    // release: luồng khác thấy con trỏ thì thấy logger đã dựng xong.
    g_global_logger.store(logger, std::memory_order_release);
}

Logger* global_logger() noexcept {
    return g_global_logger.load(std::memory_order_acquire);
}

}  // namespace orion::core
