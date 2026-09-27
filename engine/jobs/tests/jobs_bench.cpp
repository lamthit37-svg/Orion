// Benchmark của engine/jobs: chi phí lập lịch của parallel_for, độ trễ một job, và thao tác deque.
// Số đo ghi trong commit kèm preset và máy (X.8).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/detail/work_stealing_deque.hpp"
#include "engine/jobs/job_system.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

namespace orion::jobs {
namespace {

[[nodiscard]] std::unique_ptr<JobSystem> make_system(const u32 workers) {
    Result<std::unique_ptr<JobSystem>> system = JobSystem::create({.worker_count = workers});
    return system.has_value() ? std::move(*system) : nullptr;
}

// 4096 phần tử, khối 64: gần với một pha của tick zone chia theo thực thể.
void parallel_for_4096(benchmark::State& state) {
    const std::unique_ptr<JobSystem> system = make_system(static_cast<u32>(state.range(0)));
    if (system == nullptr) {
        state.SkipWithError("không tạo được job system");
        return;
    }
    JobContext context = system->external_context();
    std::atomic<u64> sink{0};
    auto body = [&sink](JobContext& /*context*/, const u32 begin, const u32 end) {
        u64 sum = 0;
        for (u32 i = begin; i < end; ++i) {
            sum += static_cast<u64>(i) * i;
        }
        sink.fetch_add(sum, std::memory_order_relaxed);
    };
    for ([[maybe_unused]] auto iteration : state) {
        context.parallel_for(4'096, 64, body);
    }
    benchmark::DoNotOptimize(sink.load());
}
BENCHMARK(parallel_for_4096)->Arg(0)->Arg(1)->Arg(3)->UseRealTime();

// Cùng cách chia nhưng mỗi phần tử tốn cỡ 100 ns (64 vòng trộn bit), gần với việc thật của một thực
// thể trong tick: đo mức tăng tốc theo số worker.
void parallel_for_4096_heavy(benchmark::State& state) {
    const std::unique_ptr<JobSystem> system = make_system(static_cast<u32>(state.range(0)));
    if (system == nullptr) {
        state.SkipWithError("không tạo được job system");
        return;
    }
    JobContext context = system->external_context();
    std::atomic<u64> sink{0};
    auto body = [&sink](JobContext& /*context*/, const u32 begin, const u32 end) {
        u64 sum = 0;
        for (u32 i = begin; i < end; ++i) {
            u64 x = i;
            for (u32 round = 0; round < 64; ++round) {
                x = (x ^ (x >> 31U)) * 0xBF58'476D'1CE4'E5B9ULL;
            }
            sum += x;
        }
        sink.fetch_add(sum, std::memory_order_relaxed);
    };
    for ([[maybe_unused]] auto iteration : state) {
        context.parallel_for(4'096, 64, body);
    }
    benchmark::DoNotOptimize(sink.load());
}
BENCHMARK(parallel_for_4096_heavy)->Arg(0)->Arg(1)->Arg(3)->UseRealTime();

// Một job rỗng: đẩy từ luồng ngoài rồi chờ; đo độ trễ khứ hồi qua worker.
void submit_and_wait_one(benchmark::State& state) {
    const std::unique_ptr<JobSystem> system = make_system(1);
    if (system == nullptr) {
        state.SkipWithError("không tạo được job system");
        return;
    }
    JobContext context = system->external_context();
    const JobFunction empty = [](JobContext& /*context*/, void* /*data*/, u32 /*begin*/,
                                 u32 /*end*/) {
    };
    for ([[maybe_unused]] auto iteration : state) {
        JobCounter counter;
        context.submit(empty, nullptr, 0, 0, counter);
        context.wait(counter);
    }
}
BENCHMARK(submit_and_wait_one)->UseRealTime();

struct Payload {
    u64 a = 0;
    u64 b = 0;
    u64 c = 0;
    u64 d = 0;
};

void deque_push_pop(benchmark::State& state) {
    detail::WorkStealingDeque<Payload> deque(1'024);
    u64 next = 0;
    for ([[maybe_unused]] auto iteration : state) {
        bool pushed = deque.push({next, next, next, next});
        std::optional<Payload> popped = deque.pop();
        benchmark::DoNotOptimize(pushed);
        benchmark::DoNotOptimize(popped);
        ++next;
    }
}
BENCHMARK(deque_push_pop);

}  // namespace
}  // namespace orion::jobs
