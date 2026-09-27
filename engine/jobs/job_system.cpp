#include "engine/jobs/job_system.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/detail/work_stealing_deque.hpp"
#include "engine/jobs/thread.hpp"

#include <array>
#include <atomic>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace orion::jobs {

namespace detail {

// Một worker: deque của nó và luồng chạy nó. Chỉ luồng của worker đẩy và lấy ở đáy deque; mọi luồng
// khác chỉ trộm (xem work_stealing_deque.hpp).
struct Worker {
    Worker(const u32 worker_index, const u32 capacity) : deque(capacity), index(worker_index) {}

    WorkStealingDeque<Job> deque;
    u32 index;
    Thread thread;
};

}  // namespace detail

JobSystem::JobSystem(Passkey /*passkey*/, const JobSystemConfig& config)
    : injected_(config.queue_capacity) {
    workers_.reserve(config.worker_count);
    for (u32 i = 0; i < config.worker_count; ++i) {
        workers_.push_back(std::make_unique<detail::Worker>(i, config.queue_capacity));
    }
}

Result<std::unique_ptr<JobSystem>> JobSystem::create(const JobSystemConfig& config) {
    auto system = std::make_unique<JobSystem>(Passkey{}, config);
    for (const std::unique_ptr<detail::Worker>& worker : system->workers_) {
        std::array<char, kMaxThreadName + 1> buffer{};
        const auto written =
            std::format_to_n(buffer.data(), kMaxThreadName, "job-{}", worker->index);
        const std::string_view name(buffer.data(), static_cast<usize>(written.size));
        Result<Thread> thread =
            Thread::start(name, [owner = system.get(), self = worker.get()](StopToken /*token*/) {
                owner->worker_main(*self);
            });
        if (!thread) {
            // Huỷ system dừng và join các worker đã chạy.
            return std::unexpected(thread.error());
        }
        worker->thread = std::move(*thread);
    }
    return system;
}

JobSystem::~JobSystem() {
    stopping_.store(true);
    events_.notify_all();
    for (const std::unique_ptr<detail::Worker>& worker : workers_) {
        worker->thread.join();
    }
}

u32 JobSystem::worker_count() const noexcept {
    return static_cast<u32>(workers_.size());
}

std::optional<Job> JobSystem::find_job(detail::Worker* self) noexcept {
    if (self != nullptr) {
        if (std::optional<Job> job = self->deque.pop()) {
            return job;
        }
    }
    Job job;
    if (injected_.try_pop(job)) {
        return job;
    }
    // Trộm vòng quanh, bắt đầu từ worker kế tiếp để các worker không cùng tranh một nạn nhân.
    const usize count = workers_.size();
    const usize start = self != nullptr ? self->index + 1 : 0;
    for (usize i = 0; i < count; ++i) {
        detail::Worker& victim = *workers_[(start + i) % count];
        if (&victim == self) {
            continue;
        }
        if (std::optional<Job> stolen = victim.deque.steal()) {
            return stolen;
        }
    }
    return std::nullopt;
}

void JobSystem::worker_main(detail::Worker& worker) noexcept {
    JobContext context(*this, &worker);
    while (true) {
        if (const std::optional<Job> job = find_job(&worker)) {
            context.run(*job);
            continue;
        }
        const u32 key = events_.prepare_wait();
        if (stopping_.load()) {
            events_.cancel_wait();
            return;
        }
        if (const std::optional<Job> job = find_job(&worker)) {
            events_.cancel_wait();
            context.run(*job);
            continue;
        }
        events_.wait(key);
    }
}

bool JobContext::enqueue(const Job& job) noexcept {
    return worker_ != nullptr ? worker_->deque.push(job) : system_->injected_.try_push(job);
}

void JobContext::run(const Job& job) noexcept {
    job.function(*this, job.data, job.begin, job.end);
    // release: công bố mọi thứ job ghi cho luồng đọc pending_ == 0 bằng acquire; các lần giảm là
    // RMW nên cùng một chuỗi release, luồng chờ đồng bộ với mọi job của nhóm. Sau lần giảm cuối,
    // counter có thể đã bị huỷ, nên chỉ còn đánh thức qua events_ của system.
    if (job.counter->pending_.fetch_sub(1, std::memory_order_release) == 1) {
        system_->events_.notify_all();
    }
}

void JobContext::submit(const JobFunction function, void* data, const u32 begin, const u32 end,
                        JobCounter& counter) noexcept {
    // relaxed: luồng wait trên counter hoặc chính là luồng submit (thứ tự chương trình), hoặc chờ
    // một job cha đang chạy và cũng được đếm trong counter, nên pending_ chưa thể về 0 trước lần
    // tăng này (các RMW trên cùng atomic có thứ tự sửa đổi theo happens-before).
    counter.pending_.fetch_add(1, std::memory_order_relaxed);
    const Job job{function, data, begin, end, &counter};
    if (!enqueue(job)) {
        run(job);
        return;
    }
    system_->events_.notify_one();
}

void JobContext::wait(JobCounter& counter) noexcept {
    while (!counter.done()) {
        if (const std::optional<Job> job = system_->find_job(worker_)) {
            run(*job);
            continue;
        }
        const u32 key = system_->events_.prepare_wait();
        if (counter.done()) {
            system_->events_.cancel_wait();
            return;
        }
        if (const std::optional<Job> job = system_->find_job(worker_)) {
            system_->events_.cancel_wait();
            run(*job);
            continue;
        }
        system_->events_.wait(key);
    }
}

}  // namespace orion::jobs
