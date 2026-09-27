#include "engine/jobs/job_system.hpp"

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <utility>
#include <vector>

namespace orion::jobs {
namespace {

[[nodiscard]] std::unique_ptr<JobSystem> make_system(const u32 workers, const u32 capacity = 256) {
    Result<std::unique_ptr<JobSystem>> system =
        JobSystem::create({.worker_count = workers, .queue_capacity = capacity});
    EXPECT_TRUE(system.has_value());
    return system.has_value() ? std::move(*system) : nullptr;
}

TEST(JobSystem, ParallelForCoversEveryIndexExactlyOnce) {
    const std::unique_ptr<JobSystem> system = make_system(3);
    ASSERT_NE(system, nullptr);
    JobContext context = system->external_context();
    for (const auto& [count, grain] : std::array<std::array<u32, 2>, 6>{
             {{0, 4}, {1, 4}, {7, 3}, {64, 64}, {1000, 7}, {5, 100}}}) {
        std::vector<std::atomic<u32>> hits(count);
        auto body = [&hits](JobContext& /*context*/, const u32 begin, const u32 end) {
            for (u32 i = begin; i < end; ++i) {
                hits[i].fetch_add(1, std::memory_order_relaxed);
            }
        };
        context.parallel_for(count, grain, body);
        for (u32 i = 0; i < count; ++i) {
            EXPECT_EQ(hits[i].load(), 1U) << "count=" << count << " grain=" << grain << " i=" << i;
        }
    }
}

// Tổng số thực theo từng khối rồi gộp theo chỉ số khối cho cùng từng bit với mọi số worker: kết
// quả không phụ thuộc lịch chạy (ADR 0007 mục 3).
[[nodiscard]] u64 chunked_sum_bits(JobSystem& system) {
    constexpr u32 kCount = 10'000;
    constexpr u32 kGrain = 37;
    constexpr u32 kChunks = (kCount + kGrain - 1) / kGrain;
    std::array<f64, kChunks> partial{};
    auto body = [&partial](JobContext& /*context*/, const u32 begin, const u32 end) {
        f64 sum = 0.0;
        for (u32 i = begin; i < end; ++i) {
            sum += 1.0 / (static_cast<f64>(i) + 0.5);
        }
        partial[begin / kGrain] = sum;
    };
    JobContext context = system.external_context();
    context.parallel_for(kCount, kGrain, body);
    f64 total = 0.0;
    for (const f64 value : partial) {
        total += value;
    }
    return std::bit_cast<u64>(total);
}

TEST(JobSystem, ChunkedResultsAreIdenticalForAnyWorkerCount) {
    const std::unique_ptr<JobSystem> none = make_system(0);
    const std::unique_ptr<JobSystem> one = make_system(1);
    const std::unique_ptr<JobSystem> four = make_system(4);
    ASSERT_TRUE(none && one && four);
    const u64 expected = chunked_sum_bits(*none);
    for (u32 repeat = 0; repeat < 20; ++repeat) {
        EXPECT_EQ(chunked_sum_bits(*one), expected);
        EXPECT_EQ(chunked_sum_bits(*four), expected);
    }
}

TEST(JobSystem, NestedParallelForRunsInsideJobs) {
    const std::unique_ptr<JobSystem> system = make_system(3);
    ASSERT_NE(system, nullptr);
    std::atomic<u64> total{0};
    auto outer = [&total](JobContext& context, const u32 begin, const u32 end) {
        for (u32 i = begin; i < end; ++i) {
            auto inner = [&total](JobContext& /*context*/, const u32 b, const u32 e) {
                total.fetch_add(e - b, std::memory_order_relaxed);
            };
            context.parallel_for(100, 9, inner);
        }
    };
    JobContext context = system->external_context();
    context.parallel_for(32, 1, outer);
    EXPECT_EQ(total.load(), 3'200U);
}

// Mỗi job chia đôi khoảng của nó cho tới khi còn một phần tử; mọi job con được đếm trong counter
// của gốc, nên wait trên gốc chỉ trả về khi cả cây xong.
struct SplitWork {
    std::atomic<u32> leaves{0};
    JobCounter* counter = nullptr;
};

void split_job(JobContext& context, void* data, const u32 begin, const u32 end) {
    auto& work = *static_cast<SplitWork*>(data);
    if (end - begin == 1) {
        work.leaves.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u32 middle = begin + ((end - begin) / 2);
    context.submit(&split_job, data, begin, middle, *work.counter);
    context.submit(&split_job, data, middle, end, *work.counter);
}

TEST(JobSystem, JobsSubmitIntoTheCounterTheyBelongTo) {
    const std::unique_ptr<JobSystem> system = make_system(3);
    ASSERT_NE(system, nullptr);
    JobCounter counter;
    SplitWork work;
    work.counter = &counter;
    JobContext context = system->external_context();
    EXPECT_FALSE(context.is_worker());
    context.submit(&split_job, &work, 0, 4'096, counter);
    context.wait(counter);
    EXPECT_TRUE(counter.done());
    EXPECT_EQ(work.leaves.load(), 4'096U);
}

TEST(JobSystem, FullQueuesRunJobsOnTheCallingThread) {
    const std::unique_ptr<JobSystem> system = make_system(1, 2);
    ASSERT_NE(system, nullptr);
    std::vector<std::atomic<u32>> hits(1'000);
    auto body = [&hits](JobContext& /*context*/, const u32 begin, const u32 end) {
        for (u32 i = begin; i < end; ++i) {
            hits[i].fetch_add(1, std::memory_order_relaxed);
        }
    };
    JobContext context = system->external_context();
    context.parallel_for(1'000, 1, body);
    for (const std::atomic<u32>& hit : hits) {
        EXPECT_EQ(hit.load(), 1U);
    }
}

TEST(JobSystem, ZeroWorkersRunEverythingOnTheWaitingThread) {
    const std::unique_ptr<JobSystem> system = make_system(0);
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->worker_count(), 0U);
    u64 total = 0;  // không cần atomic: chỉ luồng gọi chạy job
    auto body = [&total](JobContext& context, const u32 begin, const u32 end) {
        EXPECT_FALSE(context.is_worker());
        total += end - begin;
    };
    JobContext context = system->external_context();
    context.parallel_for(500, 16, body);
    EXPECT_EQ(total, 500U);
}

// Nhiều luồng ngoài (như nhiều zone trong một tiến trình world) cùng dùng một job system.
TEST(JobSystem, ManyExternalThreadsShareOneSystem) {
    const std::unique_ptr<JobSystem> system = make_system(3);
    ASSERT_NE(system, nullptr);
    std::array<std::atomic<u64>, 4> totals{};
    std::vector<Thread> zones;
    for (std::atomic<u64>& total : totals) {
        Result<Thread> zone = Thread::start("zone", [&system, &total](StopToken /*token*/) {
            JobContext context = system->external_context();
            for (u32 tick = 0; tick < 50; ++tick) {
                auto body = [&total](JobContext& /*context*/, const u32 begin, const u32 end) {
                    total.fetch_add(end - begin, std::memory_order_relaxed);
                };
                context.parallel_for(256, 8, body);
            }
        });
        ASSERT_TRUE(zone.has_value());
        zones.push_back(std::move(*zone));
    }
    for (Thread& zone : zones) {
        zone.join();
    }
    for (const std::atomic<u64>& total : totals) {
        EXPECT_EQ(total.load(), 50U * 256U);
    }
}

// X.7: hot path không cấp phát sau khi khởi động. Đo trên luồng gọi, gồm cả trường hợp luồng gọi
// tự chạy mọi job (không worker).
TEST(JobSystem, ParallelForDoesNotAllocateOnTheCallingThread) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    for (const u32 workers : {0U, 2U}) {
        const std::unique_ptr<JobSystem> system = make_system(workers);
        ASSERT_NE(system, nullptr);
        std::atomic<u64> total{0};
        auto body = [&total](JobContext& /*context*/, const u32 begin, const u32 end) {
            total.fetch_add(end - begin, std::memory_order_relaxed);
        };
        JobContext context = system->external_context();
        context.parallel_for(1'000, 10, body);
        const core::testing::AllocationScope scope;
        context.parallel_for(1'000, 10, body);
        EXPECT_EQ(scope.count(), 0U) << workers << " worker";
        EXPECT_EQ(total.load(), 2'000U);
    }
}

}  // namespace
}  // namespace orion::jobs
