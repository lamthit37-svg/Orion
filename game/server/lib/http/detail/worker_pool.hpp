#pragma once

// Nhóm worker chạy handler của server_http (ARCH §7: luồng IO không chờ DB). Hàng đợi có sức chứa
// cố định: đầy thì try_submit trả false để server trả 503, không xếp hàng vô hạn.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace orion::http::detail {

// Một việc cho worker. run chạy đúng một lần trên một worker; việc chưa chạy khi pool dừng thì bị
// huỷ mà không chạy.
class Task {
public:
    Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;
    virtual ~Task() = default;

    virtual void run() noexcept = 0;
};

// Đồng bộ: slots_, head_, size_, stopping_ dưới mutex_; ready_ báo có việc hay đang dừng.
// threads_ chỉ start và stop chạm, trên luồng sở hữu pool.
class WorkerPool {
public:
    WorkerPool() = default;
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;
    // stop().
    ~WorkerPool();

    // Tạo `workers` luồng tên "http-worker"; hàng đợi chứa tối đa `capacity` việc chưa chạy. Lỗi
    // ResourceExhausted khi không tạo được luồng (các luồng đã tạo được dừng lại). Gọi một lần.
    [[nodiscard]] Result<void> start(u32 workers, u32 capacity);

    // false khi hàng đợi đầy hay pool đang dừng; `task` khi đó bị huỷ. Gọi từ luồng nào cũng được.
    [[nodiscard]] bool try_submit(std::unique_ptr<Task> task);

    // Không nhận việc mới, huỷ việc chưa chạy, chờ việc đang chạy xong, join mọi luồng. Gọi nhiều
    // lần vô hại.
    void stop() noexcept;

private:
    void work() noexcept;

    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<std::unique_ptr<Task>> slots_;
    usize head_ = 0;
    usize size_ = 0;
    bool stopping_ = false;
    std::vector<jobs::Thread> threads_;
};

}  // namespace orion::http::detail
