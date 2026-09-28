// Tín hiệu thật của Linux: SIGTERM, SIGINT, SIGPIPE (docs/formats/service.md, mục "Dừng").

#include "engine/core/error.hpp"
#include "game/server/lib/service/stop_signal.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
// <signal.h> của POSIX: kill, sigaction, pthread_sigmask; <csignal> chỉ có phần của C chuẩn.
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>

#include <array>
#include <cerrno>

namespace orion::service {
namespace {

[[nodiscard]] bool is_pending(const int signal) {
    // sigset_t có từ <signal.h>; clang-tidy 20 đòi header nội bộ của glibc.
    // NOLINTNEXTLINE(misc-include-cleaner)
    sigset_t pending{};
    sigemptyset(&pending);
    EXPECT_EQ(sigpending(&pending), 0);
    return sigismember(&pending, signal) == 1;
}

TEST(StopSignalLinux, SigtermAndSigintEndTheWait) {
    for (const int signal : {SIGTERM, SIGINT}) {
        const Result<StopSignal> stop = StopSignal::install();
        ASSERT_TRUE(stop.has_value()) << signal;
        // Tiến trình tự gửi cho mình. install chạy trước khi test tạo luồng nào, nên mọi luồng đều
        // chặn tín hiệu này: nó không kết thúc tiến trình mà nằm chờ tới khi wait đọc.
        ASSERT_EQ(kill(getpid(), signal), 0) << signal;
        EXPECT_TRUE(is_pending(signal)) << signal;
        EXPECT_TRUE(stop->wait().has_value()) << signal;
        // wait đã đọc nó khỏi tiến trình, và nhớ rằng đã có yêu cầu dừng.
        EXPECT_FALSE(is_pending(signal)) << signal;
        EXPECT_TRUE(stop->wait().has_value()) << signal;
    }
}

TEST(StopSignalLinux, WaitLeavesOtherSignalsAlone) {
    const Result<StopSignal> stop = StopSignal::install();
    ASSERT_TRUE(stop.has_value());
    // SIGUSR1 không nằm trong tập của StopSignal: chặn riêng để nó chỉ treo, rồi thấy wait (trả về
    // nhờ request_stop) không đọc mất nó.
    // NOLINTNEXTLINE(misc-include-cleaner): như trong is_pending.
    sigset_t user{};
    sigemptyset(&user);
    sigaddset(&user, SIGUSR1);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &user, nullptr), 0);
    ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
    stop->request_stop();
    EXPECT_TRUE(stop->wait().has_value());
    EXPECT_TRUE(is_pending(SIGUSR1));
    // Dọn: đọc SIGUSR1 đang treo rồi bỏ chặn, để test sau trong cùng tiến trình không thấy nó.
    int received = 0;
    ASSERT_EQ(sigwait(&user, &received), 0);
    EXPECT_EQ(received, SIGUSR1);
    ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &user, nullptr), 0);
}

TEST(StopSignalLinux, WritingToAClosedPipeIsAnErrorNotADeath) {
    const Result<StopSignal> stop = StopSignal::install();
    ASSERT_TRUE(stop.has_value());
    struct sigaction current{};
    ASSERT_EQ(sigaction(SIGPIPE, nullptr, &current), 0);
    EXPECT_EQ(current.sa_handler, SIG_IGN);

    std::array<int, 2> fds{};
    ASSERT_EQ(pipe2(fds.data(), O_CLOEXEC), 0);
    ASSERT_EQ(close(fds[0]), 0);
    const char byte = 'x';
    EXPECT_EQ(write(fds[1], &byte, 1), -1);
    EXPECT_EQ(errno, EPIPE);
    ASSERT_EQ(close(fds[1]), 0);
}

}  // namespace
}  // namespace orion::service
