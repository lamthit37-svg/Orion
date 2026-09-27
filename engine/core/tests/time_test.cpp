#include "engine/core/time.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <thread>
#include <vector>

namespace orion::core {
namespace {

static_assert(Duration::seconds(2) == Duration::milliseconds(2'000));
static_assert(Duration::milliseconds(1'500).as_seconds() == 1);
static_assert(Duration::nanoseconds(-1'500).as_microseconds() == -1);
static_assert((Duration::seconds(3) - Duration::seconds(1)) / 2 == Duration::seconds(1));
static_assert(MonoTime::from_nanoseconds(10) + Duration::nanoseconds(5) ==
              MonoTime::from_nanoseconds(15));
static_assert(civil_from_days(0) == CivilDate{1970, 1, 1});
static_assert(civil_from_days(-1) == CivilDate{1969, 12, 31});
static_assert(WallTime::from_unix_microseconds(-1).unix_seconds() == -1);
static_assert(WallTime::from_unix_microseconds(1'999'999).unix_seconds() == 1);

std::string iso(const i64 unix_microseconds) {
    return std::format("{}", WallTime::from_unix_microseconds(unix_microseconds));
}

TEST(Time, DurationConversions) {
    const Duration d = Duration::microseconds(2'500'123);
    EXPECT_EQ(d.as_nanoseconds(), 2'500'123'000);
    EXPECT_EQ(d.as_milliseconds(), 2'500);
    EXPECT_DOUBLE_EQ(Duration::milliseconds(250).as_seconds_f64(), 0.25);
    EXPECT_LT(Duration::milliseconds(1), Duration::seconds(1));
}

TEST(Time, WallTimeFormatsAsIso8601Utc) {
    // Giá trị tham chiếu tính bằng datetime của Python (UTC).
    EXPECT_EQ(iso(0), "1970-01-01T00:00:00.000000Z");
    EXPECT_EQ(iso(1'790'504'430'123'456), "2026-09-27T10:20:30.123456Z");
    EXPECT_EQ(iso(951'868'799'000'000), "2000-02-29T23:59:59.000000Z");
    EXPECT_EQ(iso(4'107'542'400'000'000), "2100-03-01T00:00:00.000000Z");
    EXPECT_EQ(iso(-1), "1969-12-31T23:59:59.999999Z");
    EXPECT_EQ(iso(-11'670'955'200'000'000), "1600-02-29T12:00:00.000000Z");
    EXPECT_EQ(iso(-62'135'596'800'000'000), "0001-01-01T00:00:00.000000Z");
    EXPECT_EQ(iso(253'402'300'799'999'999), "9999-12-31T23:59:59.999999Z");
}

TEST(Time, WallTimeArithmetic) {
    const WallTime start = WallTime::from_unix_seconds(100);
    const WallTime later = start + Duration::milliseconds(1'500);
    EXPECT_EQ(later.unix_microseconds(), 101'500'000);
    EXPECT_EQ((later - start).as_milliseconds(), 1'500);
}

TEST(Time, SystemClocksBehave) {
    const SystemMonotonicClock mono;
    const MonoTime first = mono.now();
    const MonoTime second = mono.now();
    EXPECT_LE(first, second) << "đồng hồ đơn điệu không được lùi";
    const SystemWallClock wall;
    // 2020-01-01T00:00:00Z: đồng hồ của máy chạy test không thể ở trước mốc này.
    EXPECT_GT(wall.now(), WallTime::from_unix_seconds(1'577'836'800));
}

TEST(Time, FakeClocksOnlyMoveWhenTold) {
    FakeMonotonicClock mono{MonoTime::from_nanoseconds(5)};
    EXPECT_EQ(mono.now().nanoseconds(), 5);
    mono.advance(Duration::milliseconds(20));
    EXPECT_EQ(mono.now().nanoseconds(), 20'000'005);
    FakeWallClock wall{WallTime::from_unix_seconds(1'000)};
    wall.advance(Duration::seconds(1));
    EXPECT_EQ(wall.now(), WallTime::from_unix_seconds(1'001));
    wall.set(WallTime::from_unix_seconds(7));
    EXPECT_EQ(wall.now().unix_seconds(), 7);
}

// Test của T0 không dùng được engine/jobs (T1), nên tạo std::thread trực tiếp để TSan kiểm đồng hồ
// giả khi nhiều luồng cùng đọc và tiến nó.
TEST(Time, FakeClockIsSafeAcrossThreads) {
    FakeMonotonicClock clock;
    constexpr int kThreads = 4;
    constexpr int kSteps = 10'000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&clock] {
            for (int i = 0; i < kSteps; ++i) {
                clock.advance(Duration::nanoseconds(1));
                static_cast<void>(clock.now());
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(clock.now().nanoseconds(), i64{kThreads} * kSteps);
}

}  // namespace
}  // namespace orion::core
