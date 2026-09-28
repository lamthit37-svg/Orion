// Logger toàn cục của tiến trình server (docs/formats/service.md, mục "Log").

#include "engine/core/error.hpp"
#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "game/server/lib/service/service.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace orion::service {
namespace {

constexpr core::WallTime kStart = core::WallTime::from_unix_seconds(1'790'000'000);

// Chỉ luồng ghi log chạm sink; test đọc sau khi LogRuntime bị huỷ, tức sau khi luồng ghi đã join.
class MemorySink final : public core::LogSink {
public:
    void write(const core::LogRecord& record) noexcept override {
        messages.emplace_back(record.text());
        times.push_back(record.time);
    }
    void flush() noexcept override { ++flushes; }

    std::vector<std::string> messages;
    std::vector<core::WallTime> times;
    int flushes = 0;
};

TEST(LogRuntime, OwnsTheGlobalLoggerUntilDestroyed) {
    const core::FakeWallClock clock(kStart);
    MemorySink sink;
    {
        const Result<std::unique_ptr<LogRuntime>> runtime =
            LogRuntime::start(core::LogLevel::Info, clock, sink);
        ASSERT_TRUE(runtime.has_value());
        ASSERT_NE(core::global_logger(), nullptr);
        core::log(core::LogLevel::Info, {}, "khởi động {}", 1);
        core::log(core::LogLevel::Debug, {}, "dưới mức log");
        core::log(core::LogLevel::Error, {}, "lỗi {}", 2);
    }
    // Huỷ thì ghi nốt mọi log đã nhận, rồi gỡ logger toàn cục.
    EXPECT_EQ(core::global_logger(), nullptr);
    EXPECT_EQ(sink.messages, (std::vector<std::string>{"khởi động 1", "lỗi 2"}));
    EXPECT_EQ(sink.times, (std::vector<core::WallTime>{kStart, kStart}));
    EXPECT_GE(sink.flushes, 1);

    // Log gọi sau khi huỷ bị bỏ.
    core::log(core::LogLevel::Error, {}, "sau khi huỷ");
    EXPECT_EQ(sink.messages.size(), 2U);
}

TEST(LogRuntime, CanBeStartedAgainAfterDestruction) {
    const core::FakeWallClock clock(kStart);
    MemorySink first;
    MemorySink second;
    {
        const Result<std::unique_ptr<LogRuntime>> runtime =
            LogRuntime::start(core::LogLevel::Warn, clock, first);
        ASSERT_TRUE(runtime.has_value());
        core::log(core::LogLevel::Warn, {}, "một");
    }
    {
        const Result<std::unique_ptr<LogRuntime>> runtime =
            LogRuntime::start(core::LogLevel::Trace, clock, second);
        ASSERT_TRUE(runtime.has_value());
        core::log(core::LogLevel::Trace, {}, "hai");
    }
    EXPECT_EQ(first.messages, (std::vector<std::string>{"một"}));
    EXPECT_EQ(second.messages, (std::vector<std::string>{"hai"}));
}

}  // namespace
}  // namespace orion::service
