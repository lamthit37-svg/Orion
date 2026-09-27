#include "engine/core/log.hpp"

#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace orion::core {
namespace {

// 2026-09-27T10:20:30.123456Z
constexpr WallTime kNow = WallTime::from_unix_microseconds(1'790'504'430'123'456);

// Sink giữ bản ghi trong bộ nhớ. Chỉ luồng ghi log chạm nó, nên không cần đồng bộ.
class MemorySink final : public LogSink {
public:
    void write(const LogRecord& record) noexcept override { records.push_back(record); }
    void flush() noexcept override { ++flushes; }

    std::vector<LogRecord> records;
    int flushes = 0;
};

LogRecord make_record(const std::string_view text, const LogLevel level = LogLevel::Info) {
    LogRecord record;
    record.time = kNow;
    record.level = level;
    record.length = static_cast<u16>(text.size());
    std::ranges::copy(text, record.message.begin());
    return record;
}

std::string json(const LogRecord& record) {
    std::array<char, kLineCapacity> buffer{};
    return std::string(format_json_line(record, buffer));
}

std::string text(const LogRecord& record) {
    std::array<char, kLineCapacity> buffer{};
    return std::string(format_text_line(record, buffer));
}

TEST(Log, LevelsFilterBeforeFormatting) {
    const FakeWallClock clock{kNow};
    Logger logger{clock, {.min_level = LogLevel::Warn, .capacity = 8}};
    EXPECT_FALSE(logger.enabled(LogLevel::Info));
    EXPECT_TRUE(logger.enabled(LogLevel::Error));
    logger.write(LogLevel::Info, {}, "bị lọc {}", 1);
    logger.write(LogLevel::Error, {}, "được ghi {}", 2);
    MemorySink sink;
    EXPECT_EQ(logger.drain(sink), 1U);
    ASSERT_EQ(sink.records.size(), 1U);
    EXPECT_EQ(sink.records[0].text(), "được ghi 2");
    EXPECT_EQ(sink.records[0].time, kNow);
    EXPECT_EQ(sink.flushes, 1);
    logger.set_min_level(LogLevel::Trace);
    EXPECT_TRUE(logger.enabled(LogLevel::Trace));
}

TEST(Log, FieldsAndJsonLine) {
    LogRecord record = make_record("người chơi \"An\" vào\tzone\n");
    record.fields = {.zone = 3, .tick = 42, .entity = 7};
    EXPECT_EQ(json(record),
              R"({"ts":"2026-09-27T10:20:30.123456Z","level":"info","zone":3,"tick":42,)"
              R"("entity":7,"msg":"người chơi \"An\" vào\tzone\n"})"
              "\n");
    EXPECT_EQ(text(record),
              "2026-09-27T10:20:30.123456Z info  [zone=3 tick=42 entity=7] người chơi \"An\" "
              "vào\tzone\n\n");
}

TEST(Log, JsonReplacesBrokenUtf8AndEscapesControlBytes) {
    const LogRecord record = make_record(std::string_view("a\xFF"
                                                          "b\x01\\",
                                                          5),
                                         LogLevel::Error);
    EXPECT_EQ(json(record),
              "{\"ts\":\"2026-09-27T10:20:30.123456Z\",\"level\":\"error\","
              "\"msg\":\"a\xEF\xBF\xBD"
              "b\\u0001\\\\\"}\n");
}

TEST(Log, LongMessagesAreCutOnACodePointBoundary) {
    const FakeWallClock clock{kNow};
    Logger logger{clock, {}};
    const std::string long_text(LogRecord::kMaxMessage - 1, 'x');
    logger.write(LogLevel::Info, {}, "{}ệ", long_text);  // 'ệ' là 3 byte, vượt biên bộ đệm
    MemorySink sink;
    static_cast<void>(logger.drain(sink));
    ASSERT_EQ(sink.records.size(), 1U);
    EXPECT_TRUE(sink.records[0].truncated);
    EXPECT_EQ(sink.records[0].text(), long_text);
    EXPECT_NE(json(sink.records[0]).find(R"("truncated":true)"), std::string::npos);
}

TEST(Log, FullQueueDropsAndReportsOnce) {
    const FakeWallClock clock{kNow};
    Logger logger{clock, {.capacity = 2}};
    for (int i = 0; i < 5; ++i) {
        logger.write(LogLevel::Info, {}, "bản ghi {}", i);
    }
    EXPECT_EQ(logger.dropped(), 3U);
    MemorySink sink;
    EXPECT_EQ(logger.drain(sink), 3U);
    ASSERT_EQ(sink.records.size(), 3U);
    EXPECT_EQ(sink.records[2].level, LogLevel::Warn);
    EXPECT_EQ(sink.records[2].text(), "log: đã bỏ 3 bản ghi vì hàng đợi đầy");
    EXPECT_EQ(logger.drain(sink), 0U) << "chỉ báo một lần";
}

TEST(Log, WriteDoesNotAllocate) {
    const FakeWallClock clock{kNow};
    Logger logger{clock, {}};
    const std::string_view name = "Lâm";
    const testing::AllocationScope scope;
    for (int i = 0; i < 100; ++i) {
        logger.write(LogLevel::Info, {.zone = 1, .tick = static_cast<u64>(i)}, "{} đi {} bước",
                     name, i);
    }
    EXPECT_EQ(scope.count(), 0U);
}

// Test của T0 không dùng được engine/jobs (T1), nên luồng ghi log ở đây là std::thread.
TEST(Log, WriterThreadDeliversEverythingBeforeStopping) {
    const FakeWallClock clock{kNow};
    Logger logger{clock, {.capacity = 64}};
    MemorySink sink;
    std::thread writer([&logger, &sink] { logger.run(sink); });
    constexpr int kProducers = 3;
    constexpr int kPerProducer = 200;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&logger, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                logger.write(LogLevel::Info, {.entity = static_cast<u64>(p)}, "{}", i);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    logger.stop();
    writer.join();
    usize delivered = 0;
    std::array<int, kProducers> next{};
    for (const LogRecord& record : sink.records) {
        if (!record.fields.entity) {
            continue;  // thông báo bỏ bản ghi
        }
        const auto producer = static_cast<usize>(*record.fields.entity);
        const int value = std::stoi(std::string(record.text()));
        EXPECT_GT(value, next[producer] - 1) << "thứ tự của một luồng đẩy phải giữ nguyên";
        next[producer] = value + 1;
        ++delivered;
    }
    EXPECT_EQ(delivered + logger.dropped(), usize{kProducers} * kPerProducer);
}

TEST(Log, RateLimiterAllowsBurstPerInterval) {
    FakeMonotonicClock clock;
    LogRateLimiter limiter{clock, Duration::seconds(1), 2};
    EXPECT_TRUE(limiter.allow());
    EXPECT_TRUE(limiter.allow());
    EXPECT_FALSE(limiter.allow());
    EXPECT_FALSE(limiter.allow());
    clock.advance(Duration::milliseconds(999));
    EXPECT_FALSE(limiter.allow());
    clock.advance(Duration::milliseconds(1));
    EXPECT_TRUE(limiter.allow());
    EXPECT_EQ(limiter.take_suppressed(), 3U);
    EXPECT_EQ(limiter.take_suppressed(), 0U);
}

TEST(Log, GlobalLoggerIsOptional) {
    EXPECT_EQ(global_logger(), nullptr);
    log_info("không có logger toàn cục thì không làm gì {}", 1);
    const FakeWallClock clock{kNow};
    Logger logger{clock, {}};
    set_global_logger(&logger);
    log_warn("có logger {}", 2);
    set_global_logger(nullptr);
    MemorySink sink;
    EXPECT_EQ(logger.drain(sink), 1U);
    EXPECT_EQ(sink.records[0].text(), "có logger 2");
}

}  // namespace
}  // namespace orion::core
