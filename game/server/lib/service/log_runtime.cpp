// Logger toàn cục của tiến trình server, với một luồng ghi riêng (ADR 0007).

#include "engine/core/error.hpp"
#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "engine/jobs/thread.hpp"
#include "game/server/lib/service/service.hpp"

#include <expected>
#include <memory>
#include <utility>

namespace orion::service {

LogRuntime::LogRuntime(Token /*token*/, const core::LogLevel level, const core::WallClock& clock)
    : logger_(clock, core::Logger::Config{.min_level = level}) {}

Result<std::unique_ptr<LogRuntime>> LogRuntime::start(const core::LogLevel level,
                                                      const core::WallClock& clock,
                                                      core::LogSink& sink) {
    auto runtime = std::make_unique<LogRuntime>(Token{}, level, clock);
    // Đặt logger toàn cục trước khi tạo luồng ghi (engine/core/log.hpp); log gọi trước khi luồng
    // chạy nằm trong hàng đợi của logger.
    core::set_global_logger(&runtime->logger_);
    core::Logger* const logger = &runtime->logger_;
    Result<jobs::Thread> thread = jobs::Thread::start(
        "log-writer", [logger, &sink](jobs::StopToken /*token*/) { logger->run(sink); });
    if (!thread) {
        // ~LogRuntime gỡ logger toàn cục; join của luồng chưa tạo không làm gì.
        return std::unexpected(thread.error());
    }
    runtime->thread_ = std::move(*thread);
    return runtime;
}

LogRuntime::~LogRuntime() {
    // Gỡ trước khi dừng: log gọi sau đó bị bỏ, thay vì vào hàng đợi mà không còn ai ghi ra.
    core::set_global_logger(nullptr);
    logger_.stop();
    thread_.join();
}

}  // namespace orion::service
