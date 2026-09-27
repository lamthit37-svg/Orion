#pragma once

// Eventcount: cho luồng ngủ tới khi có việc mới mà không bỏ lỡ tín hiệu, và không gọi hệ điều hành
// khi không ai ngủ.
//
// Cách dùng của luồng chờ:
//     const u32 key = events.prepare_wait();
//     if (<điều kiện đã đúng>) { events.cancel_wait(); ... } else { events.wait(key); }
// Luồng báo: làm cho điều kiện đúng (ví dụ đẩy job), rồi gọi notify_one() hoặc notify_all().
//
// Đồng bộ: mọi thao tác là seq_cst. Nếu notify đọc sleepers_ = 0 thì trong thứ tự toàn cục, lần
// tăng epoch_ của nó đứng trước prepare_wait của luồng chờ, nên luồng chờ kiểm lại điều kiện và
// thấy thay đổi; ngược lại notify thấy có người ngủ và đánh thức, hoặc epoch_ đã khác key nên wait
// trả về ngay.

#include "engine/core/types.hpp"

#include <atomic>

namespace orion::jobs::detail {

class EventCount {
public:
    [[nodiscard]] u32 prepare_wait() noexcept {
        sleepers_.fetch_add(1);
        return epoch_.load();
    }

    void cancel_wait() noexcept { sleepers_.fetch_sub(1); }

    // Ngủ tới khi epoch_ khác key (có thể thức sớm; bên gọi luôn kiểm lại điều kiện).
    void wait(const u32 key) noexcept {
        epoch_.wait(key);
        sleepers_.fetch_sub(1);
    }

    void notify_one() noexcept {
        epoch_.fetch_add(1);
        if (sleepers_.load() != 0) {
            epoch_.notify_one();
        }
    }

    void notify_all() noexcept {
        epoch_.fetch_add(1);
        if (sleepers_.load() != 0) {
            epoch_.notify_all();
        }
    }

private:
    std::atomic<u32> epoch_{0};
    std::atomic<u32> sleepers_{0};
};

}  // namespace orion::jobs::detail
