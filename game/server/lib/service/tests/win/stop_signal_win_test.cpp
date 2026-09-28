// Event dừng có tên của Windows (docs/formats/service.md, mục "Dừng"). Không gửi sự kiện console
// thật: GenerateConsoleCtrlEvent đi tới mọi tiến trình dùng chung console, kể cả ctest; test mở
// event theo tên như handler console làm.

#include "engine/core/error.hpp"
#include "game/server/lib/service/stop_signal.hpp"

#include <gtest/gtest.h>
#include <windows.h>

#include <format>
#include <string>

namespace orion::service {
namespace {

[[nodiscard]] std::wstring event_name() {
    return std::format(L"Local\\orion-stop-{}", GetCurrentProcessId());
}

TEST(StopSignalWindows, SettingTheNamedEventEndsTheWait) {
    const Result<StopSignal> stop = StopSignal::install();
    ASSERT_TRUE(stop.has_value());
    const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name().c_str());
    ASSERT_NE(event, nullptr);
    EXPECT_NE(SetEvent(event), FALSE);
    EXPECT_NE(CloseHandle(event), FALSE);
    EXPECT_TRUE(stop->wait().has_value());
    EXPECT_TRUE(stop->wait().has_value());
}

TEST(StopSignalWindows, NameTakenBeforeInstallIsRefused) {
    const HANDLE squatter = CreateEventW(nullptr, TRUE, FALSE, event_name().c_str());
    ASSERT_NE(squatter, nullptr);
    const Result<StopSignal> refused = StopSignal::install();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::AlreadyExists);
    EXPECT_NE(CloseHandle(squatter), FALSE);
    // Tên được trả lại khi handle cuối cùng đóng.
    EXPECT_TRUE(StopSignal::install().has_value());
}

}  // namespace
}  // namespace orion::service
