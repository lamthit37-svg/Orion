#include "game/server/lib/http/detail/worker_pool.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <expected>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace orion::http::detail {

WorkerPool::~WorkerPool() {
    stop();
}

Result<void> WorkerPool::start(const u32 workers, const u32 capacity) {
    ORION_ASSERT(workers > 0 && capacity > 0 && threads_.empty(), "http: WorkerPool::start sai");
    slots_.resize(capacity);
    threads_.reserve(workers);
    for (u32 i = 0; i < workers; ++i) {
        Result<jobs::Thread> thread =
            jobs::Thread::start("http-worker", [this](jobs::StopToken) { work(); });
        if (!thread) {
            stop();
            return std::unexpected(thread.error());
        }
        threads_.push_back(std::move(*thread));
    }
    return {};
}

bool WorkerPool::try_submit(std::unique_ptr<Task> task) {
    {
        const std::scoped_lock lock(mutex_);
        if (stopping_ || size_ == slots_.size()) {
            return false;
        }
        slots_[(head_ + size_) % slots_.size()] = std::move(task);
        ++size_;
    }
    ready_.notify_one();
    return true;
}

void WorkerPool::work() noexcept {
    std::unique_lock lock(mutex_);
    for (;;) {
        ready_.wait(lock, [this] { return stopping_ || size_ > 0; });
        if (stopping_) {
            return;
        }
        std::unique_ptr<Task> task = std::move(slots_[head_]);
        head_ = (head_ + 1) % slots_.size();
        --size_;
        lock.unlock();
        task->run();
        // Huỷ việc ngoài khoá: nó có thể thả kết nối cuối cùng của mình.
        task.reset();
        lock.lock();
    }
}

void WorkerPool::stop() noexcept {
    std::vector<std::unique_ptr<Task>> dropped;
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
        // Huỷ việc chưa chạy ngoài khoá: huỷ một việc có thể thả kết nối cuối cùng của nó.
        for (; size_ > 0; --size_) {
            dropped.push_back(std::move(slots_[head_]));
            head_ = (head_ + 1) % slots_.size();
        }
    }
    ready_.notify_all();
    for (jobs::Thread& thread : threads_) {
        thread.join();
    }
    threads_.clear();
}

}  // namespace orion::http::detail
