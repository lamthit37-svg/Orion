#include "game/server/lib/service/stop_signal.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <windows.h>

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace orion::service {
namespace detail {

// Event reset tay, có tên theo id tiến trình. Handler console của hệ điều hành không có tham số ngữ
// cảnh, nên nó mở event theo tên thay vì đọc một biến toàn cục (CLAUDE.md X.3). Event reset tay
// giữ trạng thái đã báo, nên mọi lần wait sau yêu cầu dừng đầu tiên trả về ngay.
struct StopSignalState {
    HANDLE event = nullptr;
    bool handler_installed = false;

    StopSignalState() = default;
    StopSignalState(const StopSignalState&) = delete;
    StopSignalState& operator=(const StopSignalState&) = delete;
    StopSignalState(StopSignalState&&) = delete;
    StopSignalState& operator=(StopSignalState&&) = delete;
    ~StopSignalState();
};

}  // namespace detail

namespace {

[[nodiscard]] std::wstring event_name() {
    return std::format(L"Local\\orion-stop-{}", GetCurrentProcessId());
}

// Hệ điều hành tạo một luồng mới trong tiến trình để gọi hàm này (tài liệu HandlerRoutine của
// Microsoft). Ctrl+C, Ctrl+Break: trả về ngay, tiến trình chạy tiếp và main dừng êm. Đóng console,
// đăng xuất, tắt máy: hệ điều hành kết thúc tiến trình khi hàm này trả về TRUE hay khi hết thời
// gian chờ (5 giây với đóng console), nên giữ luồng ở đây cho main dừng êm trong khoảng đó; main
// trả về thì tiến trình kết thúc, cả luồng này (docs/formats/service.md, mục "Dừng").
BOOL WINAPI on_console_event(const DWORD type) noexcept {
    const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name().c_str());
    if (event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        Sleep(INFINITE);
    }
    return TRUE;
}

}  // namespace

detail::StopSignalState::~StopSignalState() {
    if (handler_installed) {
        SetConsoleCtrlHandler(on_console_event, FALSE);
    }
    if (event != nullptr) {
        CloseHandle(event);
    }
}

Result<StopSignal> StopSignal::install() noexcept {
    auto state = std::make_unique<detail::StopSignalState>();
    state->event = CreateEventW(nullptr, TRUE, FALSE, event_name().c_str());
    if (state->event == nullptr) {
        return fail(ErrorCode::Io, "service: không tạo được event dừng",
                    static_cast<i64>(GetLastError()));
    }
    // Tên đã có: tiến trình khác tạo trước và giữ được quyền báo event này, hay một StopSignal khác
    // của tiến trình này còn sống.
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return fail(ErrorCode::AlreadyExists, "service: event dừng đã tồn tại");
    }
    if (SetConsoleCtrlHandler(on_console_event, TRUE) == FALSE) {
        return fail(ErrorCode::Io, "service: không cài được handler console",
                    static_cast<i64>(GetLastError()));
    }
    state->handler_installed = true;
    return StopSignal(std::move(state));
}

Result<void> StopSignal::wait() const noexcept {
    if (WaitForSingleObject(state_->event, INFINITE) != WAIT_OBJECT_0) {
        return fail(ErrorCode::Io, "service: không chờ được event dừng",
                    static_cast<i64>(GetLastError()));
    }
    return {};
}

void StopSignal::request_stop() const noexcept {
    SetEvent(state_->event);
}

StopSignal::StopSignal(std::unique_ptr<detail::StopSignalState> state) noexcept
    : state_(std::move(state)) {}
StopSignal::StopSignal(StopSignal&&) noexcept = default;
StopSignal& StopSignal::operator=(StopSignal&&) noexcept = default;
StopSignal::~StopSignal() = default;

}  // namespace orion::service
