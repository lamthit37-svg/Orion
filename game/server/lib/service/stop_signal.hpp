#pragma once

// Tín hiệu dừng của một tiến trình server (docs/formats/service.md, mục "Dừng"): SIGINT, SIGTERM
// trên Linux; Ctrl+C, Ctrl+Break, đóng console, đăng xuất, tắt máy trên Windows. Cài trong linux/
// và win/ (CLAUDE.md X.2). Không có trạng thái toàn cục nào trong code của dự án (X.3): Linux chặn
// hai tín hiệu rồi đọc chúng qua signalfd; Windows dùng một event có tên theo id tiến trình, mà
// handler console (hệ điều hành gọi trên luồng riêng của nó) mở theo tên.

#include "engine/core/error.hpp"

#include <memory>

namespace orion::service {

namespace detail {
struct StopSignalState;
}  // namespace detail

class StopSignal {
public:
    // Gọi đầu tiên trong main, trước khi tạo luồng nào: trên Linux mọi luồng tạo sau đó thừa hưởng
    // việc chặn SIGINT, SIGTERM, nên hai tín hiệu này chỉ tới wait; SIGPIPE bị bỏ qua cho cả tiến
    // trình, để ghi vào socket hay pipe đã đóng là lỗi EPIPE chứ không kết thúc tiến trình. Mỗi lúc
    // chỉ một StopSignal sống trong một tiến trình. Lỗi: Io khi hệ điều hành từ chối; AlreadyExists
    // (Windows) khi tên event đã có, do tiến trình khác tạo trước hay do một StopSignal khác còn
    // sống.
    [[nodiscard]] static Result<StopSignal> install() noexcept;

    StopSignal(StopSignal&&) noexcept;
    StopSignal& operator=(StopSignal&&) noexcept;
    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;
    ~StopSignal();

    // Chặn tới khi tiến trình nhận tín hiệu dừng, hay request_stop được gọi. Một khi đã có yêu cầu
    // dừng thì mọi lần gọi sau trả về ngay. Gọi từ một luồng tại một thời điểm. Lỗi: Io khi hệ điều
    // hành không cho chờ nữa; bên gọi dừng tiến trình như khi có tín hiệu dừng.
    [[nodiscard]] Result<void> wait() const noexcept;
    // Như thể tiến trình nhận tín hiệu dừng: cho lệnh dừng của chính server và cho test. Gọi từ
    // luồng nào cũng được.
    void request_stop() const noexcept;

private:
    explicit StopSignal(std::unique_ptr<detail::StopSignalState> state) noexcept;

    std::unique_ptr<detail::StopSignalState> state_;
};

}  // namespace orion::service
