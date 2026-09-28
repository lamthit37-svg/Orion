#include "game/server/lib/service/stop_signal.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

// <signal.h> của POSIX: sigaction, pthread_sigmask, sigset_t; <csignal> chỉ có phần của C chuẩn.
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <memory>
#include <utility>

namespace orion::service {
namespace detail {

// signal_fd chỉ luồng gọi wait đọc. event_fd: request_stop ghi từ luồng nào cũng được (write(2) lên
// eventfd là một thao tác nguyên tử của kernel), wait chỉ poll mà không đọc, nên một khi bộ đếm
// của nó khác 0 thì yêu cầu dừng còn mãi.
struct StopSignalState {
    int signal_fd = -1;
    int event_fd = -1;

    StopSignalState() = default;
    StopSignalState(const StopSignalState&) = delete;
    StopSignalState& operator=(const StopSignalState&) = delete;
    StopSignalState(StopSignalState&&) = delete;
    StopSignalState& operator=(StopSignalState&&) = delete;
    ~StopSignalState() {
        if (signal_fd >= 0) {
            static_cast<void>(close(signal_fd));
        }
        if (event_fd >= 0) {
            static_cast<void>(close(event_fd));
        }
    }
};

}  // namespace detail

namespace {

void latch(const int event_fd) noexcept {
    const u64 one = 1;
    // write chỉ lỗi (EAGAIN) khi bộ đếm sắp tràn, tức nó đã khác 0: yêu cầu dừng vẫn còn đó.
    static_cast<void>(write(event_fd, &one, sizeof(one)));
}

}  // namespace

Result<StopSignal> StopSignal::install() noexcept {
    // sigset_t có từ <signal.h>; clang-tidy 20 đòi header nội bộ của glibc.
    // NOLINTNEXTLINE(misc-include-cleaner)
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    // Chặn ở luồng này, và vì chưa có luồng nào khác, ở mọi luồng tạo sau: hai tín hiệu không bao
    // giờ chạy handler hay hành động mặc định, mà nằm chờ tới khi wait đọc chúng qua signalfd.
    if (const int error = pthread_sigmask(SIG_BLOCK, &signals, nullptr); error != 0) {
        return fail(ErrorCode::Io, "service: không chặn được SIGINT, SIGTERM", error);
    }
    // Ghi vào socket hay pipe đã đóng (log ra stdout khi bên đọc đã thoát) trả EPIPE cho bên gọi xử
    // lý, thay vì SIGPIPE kết thúc tiến trình.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, nullptr) != 0) {
        return fail(ErrorCode::Io, "service: không bỏ qua được SIGPIPE", errno);
    }
    auto state = std::make_unique<detail::StopSignalState>();
    state->signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (state->signal_fd < 0) {
        return fail(ErrorCode::Io, "service: không tạo được signalfd", errno);
    }
    state->event_fd = eventfd(0, EFD_CLOEXEC);
    if (state->event_fd < 0) {
        return fail(ErrorCode::Io, "service: không tạo được eventfd", errno);
    }
    return StopSignal(std::move(state));
}

Result<void> StopSignal::wait() const noexcept {
    std::array<pollfd, 2> fds{{
        {.fd = state_->signal_fd, .events = POLLIN, .revents = 0},
        {.fd = state_->event_fd, .events = POLLIN, .revents = 0},
    }};
    while (poll(fds.data(), fds.size(), -1) < 0) {
        // EINTR: một tín hiệu khác (không phải hai tín hiệu dừng, chúng bị chặn) ngắt poll.
        if (errno != EINTR) {
            return fail(ErrorCode::Io, "service: không chờ được tín hiệu dừng", errno);
        }
    }
    if ((fds[0].revents & POLLIN) != 0) {
        // Đọc để tín hiệu không còn treo ở tiến trình, rồi chốt vào eventfd để lần wait sau cũng
        // trả về ngay.
        signalfd_siginfo info{};
        static_cast<void>(read(state_->signal_fd, &info, sizeof(info)));
        latch(state_->event_fd);
    }
    return {};
}

void StopSignal::request_stop() const noexcept {
    latch(state_->event_fd);
}

StopSignal::StopSignal(std::unique_ptr<detail::StopSignalState> state) noexcept
    : state_(std::move(state)) {}
StopSignal::StopSignal(StopSignal&&) noexcept = default;
StopSignal& StopSignal::operator=(StopSignal&&) noexcept = default;
StopSignal::~StopSignal() = default;

}  // namespace orion::service
