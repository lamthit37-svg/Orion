#include "engine/jobs/thread.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <string_view>
#include <thread>
#include <utility>

namespace orion::jobs {
namespace {

TEST(Thread, RunsBodyAndJoins) {
    std::atomic<u32> runs{0};
    Result<Thread> thread =
        Thread::start("orion-test", [&runs](StopToken /*token*/) { runs.fetch_add(1); });
    ASSERT_TRUE(thread.has_value());
    EXPECT_TRUE(thread->joinable());
    EXPECT_EQ(thread->name(), "orion-test");
    thread->join();
    EXPECT_FALSE(thread->joinable());
    EXPECT_EQ(thread->name(), "");
    EXPECT_EQ(runs.load(), 1U);
    thread->join();  // lần thứ hai không làm gì
}

TEST(Thread, NameIsVisibleToTheOperatingSystem) {
    std::array<char, kMaxThreadName + 1> seen{};
    Result<Thread> thread = Thread::start("zone-12", [&seen](StopToken /*token*/) {
        std::array<char, kMaxThreadName + 1> buffer{};
        const std::string_view name = current_thread_name(buffer);
        std::ranges::copy(name, seen.begin());
    });
    ASSERT_TRUE(thread.has_value());
    thread->join();
    EXPECT_EQ(std::string_view(seen.data()), "zone-12");
}

TEST(Thread, LongestNameFits) {
    std::array<char, kMaxThreadName + 1> seen{};
    Result<Thread> thread = Thread::start("abcdefghijklmno", [&seen](StopToken /*token*/) {
        std::array<char, kMaxThreadName + 1> buffer{};
        std::ranges::copy(current_thread_name(buffer), seen.begin());
    });
    ASSERT_TRUE(thread.has_value());
    thread->join();
    EXPECT_EQ(std::string_view(seen.data()), "abcdefghijklmno");
}

TEST(Thread, DestructorRequestsStopAndJoins) {
    std::atomic<bool> finished{false};
    {
        const Result<Thread> thread = Thread::start("stopper", [&finished](const StopToken token) {
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            finished.store(true);
        });
        ASSERT_TRUE(thread.has_value());
    }
    EXPECT_TRUE(finished.load());
}

TEST(Thread, MoveTransfersOwnership) {
    std::atomic<u32> runs{0};
    auto body = [&runs](const StopToken token) {
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        runs.fetch_add(1);
    };
    Result<Thread> first = Thread::start("first", body);
    Result<Thread> second = Thread::start("second", body);
    ASSERT_TRUE(first.has_value() && second.has_value());
    Thread moved(std::move(*first));
    EXPECT_FALSE(first->joinable());  // NOLINT(bugprone-use-after-move): kiểm trạng thái sau move
    EXPECT_EQ(moved.name(), "first");
    // Gán đè dừng và join luồng cũ của `moved` trước khi nhận luồng mới.
    moved = std::move(*second);
    EXPECT_EQ(runs.load(), 1U);
    EXPECT_EQ(moved.name(), "second");
    moved.request_stop();
    moved.join();
    EXPECT_EQ(runs.load(), 2U);
}

TEST(Thread, HardwareThreadCountIsPositive) {
    EXPECT_GE(hardware_thread_count(), 1U);
}

#if !ORION_SHIP
TEST(ThreadDeathTest, InvalidNamesAreProgrammingErrors) {
    const auto empty_body = [](StopToken /*token*/) {
    };
    EXPECT_DEATH(static_cast<void>(Thread::start("", empty_body).has_value()), "tên luồng");
    EXPECT_DEATH(static_cast<void>(Thread::start("abcdefghijklmnop", empty_body).has_value()),
                 "tên luồng");
    EXPECT_DEATH(static_cast<void>(Thread::start("tab\there", empty_body).has_value()),
                 "tên luồng");
}
#endif

}  // namespace
}  // namespace orion::jobs
