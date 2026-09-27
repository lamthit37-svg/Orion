#pragma once

// Job system work-stealing (ARCH §4.1, ADR 0007): chạy các pha song song của tick zone và việc song
// song của client, tool.
//
// - Mỗi worker có một deque Chase–Lev; job một worker đẩy ra nằm trong deque của nó, worker rảnh
//   trộm từ deque của worker khác. Luồng ngoài (luồng mô phỏng của zone, luồng chính của client)
//   đẩy vào một hàng đợi chung.
// - Không cấp phát và không khoá sau khi dựng (X.7). Hàng đợi đầy thì job chạy ngay trên luồng gọi,
//   nên submit không bao giờ chặn và không bao giờ thất bại.
// - Luồng gọi wait không ngồi không: nó chạy job khác cho tới khi nhóm của nó xong.
// - parallel_for chia việc thành các khối cố định theo grain, không phụ thuộc số worker hay lịch
//   chạy. Mỗi khối ghi kết quả vào chỗ riêng theo chỉ số khối, luồng gọi gộp theo thứ tự chỉ số,
//   nên kết quả tất định (ADR 0007 mục 3).

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/queue.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/detail/event_count.hpp"
#include "engine/jobs/thread.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace orion::jobs {

class JobContext;
class JobSystem;

// Số job chưa xong của một nhóm. Phải sống tới khi wait trên nó trả về (thường nằm trên stack của
// luồng gọi wait). Đồng bộ: job giảm pending_ bằng acq_rel khi xong; wait đọc bằng acquire, nên
// sau khi wait trả về, luồng gọi thấy mọi thứ các job đã ghi.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
    JobCounter(JobCounter&&) = delete;
    JobCounter& operator=(JobCounter&&) = delete;
    ~JobCounter() = default;

    [[nodiscard]] bool done() const noexcept {
        return pending_.load(std::memory_order_acquire) == 0;
    }

private:
    friend class JobContext;
    friend class JobSystem;

    std::atomic<u32> pending_{0};
};

// Hàm của một job: chạy phần [begin, end) của việc mô tả bởi data.
using JobFunction = void (*)(JobContext& context, void* data, u32 begin, u32 end);

struct Job {
    JobFunction function = nullptr;
    void* data = nullptr;
    u32 begin = 0;
    u32 end = 0;
    JobCounter* counter = nullptr;
};

namespace detail {
struct Worker;
}  // namespace detail

// Luồng đang dùng job system: một worker, hoặc một luồng ngoài lấy từ JobSystem::external_context.
// Không dùng chung giữa các luồng.
class JobContext {
public:
    // Đẩy job chạy function(context, data, begin, end); counter tăng trước khi job có thể chạy.
    void submit(JobFunction function, void* data, u32 begin, u32 end, JobCounter& counter) noexcept;

    // Chờ counter về 0; trong lúc chờ, chạy job khác.
    void wait(JobCounter& counter) noexcept;

    // Chạy fn(context, begin, end) trên các khối [k·grain, min((k+1)·grain, count)) song song và
    // trả về khi mọi khối xong. Khối k luôn là cùng một khoảng, dù có bao nhiêu worker. fn phải
    // sống tới khi hàm trả về (nó nằm trên stack của luồng gọi).
    template <class F>
    void parallel_for(u32 count, u32 grain, F& fn) noexcept;

    [[nodiscard]] JobSystem& system() const noexcept { return *system_; }
    [[nodiscard]] bool is_worker() const noexcept { return worker_ != nullptr; }

private:
    friend class JobSystem;
    JobContext(JobSystem& system, detail::Worker* worker) noexcept
        : system_(&system), worker_(worker) {}

    // Đưa job vào hàng đợi mà không đánh thức ai; false khi đầy.
    [[nodiscard]] bool enqueue(const Job& job) noexcept;
    void run(const Job& job) noexcept;

    JobSystem* system_;
    detail::Worker* worker_;  // nullptr với luồng ngoài
};

struct JobSystemConfig {
    // Số worker. 0 cũng hợp lệ: job chỉ chạy khi luồng gọi wait (dùng cho test và máy một nhân).
    u32 worker_count = 1;
    // Sức chứa deque của mỗi worker và của hàng đợi chung, làm tròn lên lũy thừa của 2.
    u32 queue_capacity = 4'096;
};

// Đồng bộ (X.7): workers_ không đổi sau create nên mọi luồng đọc tự do; deque của mỗi worker theo
// luật trong work_stealing_deque.hpp; injected_ là MpmcRing không khoá; events_ và stopping_ dùng
// seq_cst (xem event_count.hpp). Không có mutex nào.
class JobSystem {
    struct Passkey {};

public:
    // Dựng và khởi động worker (tên "job-0", "job-1", ...). Trả ResourceExhausted nếu không tạo
    // được luồng.
    [[nodiscard]] static Result<std::unique_ptr<JobSystem>> create(const JobSystemConfig& config);

    // Chỉ create gọi được (Passkey riêng); công khai để std::make_unique dùng.
    JobSystem(Passkey passkey, const JobSystemConfig& config);

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;
    // Dừng và join mọi worker. Tiền điều kiện: mọi nhóm job đã được wait xong.
    ~JobSystem();

    // Ngữ cảnh cho một luồng ngoài; job nó đẩy vào hàng đợi chung.
    [[nodiscard]] JobContext external_context() noexcept { return {*this, nullptr}; }

    [[nodiscard]] u32 worker_count() const noexcept;

private:
    friend class JobContext;

    [[nodiscard]] std::optional<Job> find_job(detail::Worker* self) noexcept;
    void worker_main(detail::Worker& worker) noexcept;

    std::vector<std::unique_ptr<detail::Worker>> workers_;
    core::MpmcRing<Job> injected_;
    detail::EventCount events_;
    std::atomic<bool> stopping_{false};
};

template <class F>
void JobContext::parallel_for(const u32 count, const u32 grain, F& fn) noexcept {
    ORION_ASSERT(grain > 0, "parallel_for cần grain > 0");
    const JobFunction invoke = [](JobContext& context, void* data, const u32 begin, const u32 end) {
        (*static_cast<F*>(data))(context, begin, end);
    };
    JobCounter counter;
    for (u64 begin = 0; begin < count; begin += grain) {
        const u64 end = begin + grain < count ? begin + grain : count;
        const Job job{invoke, &fn, static_cast<u32>(begin), static_cast<u32>(end), &counter};
        // relaxed: xem submit.
        counter.pending_.fetch_add(1, std::memory_order_relaxed);
        if (!enqueue(job)) {
            run(job);
        }
    }
    system_->events_.notify_all();
    wait(counter);
}

}  // namespace orion::jobs
