// Tín hiệu dừng, phần chung của mọi nền tảng (docs/formats/service.md, mục "Dừng"). Tín hiệu thật
// của hệ điều hành: tests/linux/.

#include "game/server/lib/service/stop_signal.hpp"

#include "engine/core/error.hpp"
#include "engine/jobs/thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <utility>

namespace orion::service {
namespace {

TEST(StopSignal, RequestStopWakesTheWaitingThread) {
    const Result<StopSignal> stop = StopSignal::install();
    ASSERT_TRUE(stop.has_value());
    std::atomic<bool> woke{false};
    Result<jobs::Thread> waiter = jobs::Thread::start("stop-waiter", [&](jobs::StopToken) {
        EXPECT_TRUE(stop->wait().has_value());
        woke.store(true);
    });
    ASSERT_TRUE(waiter.has_value());
    stop->request_stop();
    waiter->join();
    EXPECT_TRUE(woke.load());
}

TEST(StopSignal, StopRequestIsNeverLostAndStaysSet) {
    const Result<StopSignal> stop = StopSignal::install();
    ASSERT_TRUE(stop.has_value());
    // Yêu cầu trước khi có ai chờ: wait sau đó vẫn trả về, và mọi lần wait sau nữa cũng vậy.
    stop->request_stop();
    stop->request_stop();
    EXPECT_TRUE(stop->wait().has_value());
    EXPECT_TRUE(stop->wait().has_value());
}

TEST(StopSignal, WorksAfterBeingMoved) {
    Result<StopSignal> installed = StopSignal::install();
    ASSERT_TRUE(installed.has_value());
    const StopSignal stop = std::move(*installed);
    stop.request_stop();
    EXPECT_TRUE(stop.wait().has_value());
}

TEST(StopSignal, CanBeInstalledAgainAfterDestruction) {
    for (int round = 0; round < 3; ++round) {
        const Result<StopSignal> stop = StopSignal::install();
        ASSERT_TRUE(stop.has_value()) << round;
        stop->request_stop();
        EXPECT_TRUE(stop->wait().has_value()) << round;
    }
}

}  // namespace
}  // namespace orion::service
