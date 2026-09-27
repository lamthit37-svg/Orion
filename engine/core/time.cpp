#include "engine/core/time.hpp"

#include <chrono>

namespace orion::core {

MonoTime SystemMonotonicClock::now() const noexcept {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return MonoTime::from_nanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

WallTime SystemWallClock::now() const noexcept {
    // system_clock đo từ Unix epoch từ C++20.
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return WallTime::from_unix_microseconds(
        std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count());
}

}  // namespace orion::core
