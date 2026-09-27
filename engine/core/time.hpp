#pragma once

// Thời gian và đồng hồ (ADR 0002, CLAUDE.md X.4).
//
// - Duration: khoảng thời gian có dấu, độ phân giải nano giây, đủ ±292 năm.
// - MonoTime: mốc trên đồng hồ đơn điệu, dùng để đo và lập lịch (ví dụ vòng tick của zone).
// - WallTime: giờ UTC tính bằng micro giây từ Unix epoch, chỉ dùng cho hạn token, log, dấu thời
//   gian trong DB và kiểm toán (ADR 0002 mục 8).
//
// Header không include <chrono>, để game/shared dùng được các kiểu giá trị này (X.11); chỉ
// time.cpp chạm std::chrono. Đồng hồ là interface để tiêm được đồng hồ giả trong test (X.4).

#include "engine/core/types.hpp"

#include <atomic>
#include <compare>
#include <format>
#include <string_view>

namespace orion::core {

class Duration {
public:
    constexpr Duration() noexcept = default;

    [[nodiscard]] static constexpr Duration nanoseconds(const i64 value) noexcept {
        return Duration{value};
    }
    [[nodiscard]] static constexpr Duration microseconds(const i64 value) noexcept {
        return Duration{value * 1'000};
    }
    [[nodiscard]] static constexpr Duration milliseconds(const i64 value) noexcept {
        return Duration{value * 1'000'000};
    }
    [[nodiscard]] static constexpr Duration seconds(const i64 value) noexcept {
        return Duration{value * 1'000'000'000};
    }

    // Các phép đổi đơn vị cắt về phía 0, như phép chia số nguyên.
    [[nodiscard]] constexpr i64 as_nanoseconds() const noexcept { return ns_; }
    [[nodiscard]] constexpr i64 as_microseconds() const noexcept { return ns_ / 1'000; }
    [[nodiscard]] constexpr i64 as_milliseconds() const noexcept { return ns_ / 1'000'000; }
    [[nodiscard]] constexpr i64 as_seconds() const noexcept { return ns_ / 1'000'000'000; }
    [[nodiscard]] constexpr f64 as_seconds_f64() const noexcept {
        return static_cast<f64>(ns_) / 1e9;
    }

    friend constexpr Duration operator+(const Duration a, const Duration b) noexcept {
        return Duration{a.ns_ + b.ns_};
    }
    friend constexpr Duration operator-(const Duration a, const Duration b) noexcept {
        return Duration{a.ns_ - b.ns_};
    }
    friend constexpr Duration operator*(const Duration a, const i64 factor) noexcept {
        return Duration{a.ns_ * factor};
    }
    friend constexpr Duration operator/(const Duration a, const i64 divisor) noexcept {
        return Duration{a.ns_ / divisor};
    }
    friend constexpr auto operator<=>(Duration, Duration) noexcept = default;

private:
    constexpr explicit Duration(const i64 ns) noexcept : ns_(ns) {}

    i64 ns_ = 0;
};

class MonoTime {
public:
    constexpr MonoTime() noexcept = default;

    [[nodiscard]] static constexpr MonoTime from_nanoseconds(const i64 value) noexcept {
        return MonoTime{value};
    }
    [[nodiscard]] constexpr i64 nanoseconds() const noexcept { return ns_; }

    friend constexpr Duration operator-(const MonoTime a, const MonoTime b) noexcept {
        return Duration::nanoseconds(a.ns_ - b.ns_);
    }
    friend constexpr MonoTime operator+(const MonoTime a, const Duration d) noexcept {
        return MonoTime{a.ns_ + d.as_nanoseconds()};
    }
    friend constexpr auto operator<=>(MonoTime, MonoTime) noexcept = default;

private:
    constexpr explicit MonoTime(const i64 ns) noexcept : ns_(ns) {}

    i64 ns_ = 0;
};

class WallTime {
public:
    constexpr WallTime() noexcept = default;

    [[nodiscard]] static constexpr WallTime from_unix_microseconds(const i64 value) noexcept {
        return WallTime{value};
    }
    [[nodiscard]] static constexpr WallTime from_unix_seconds(const i64 value) noexcept {
        return WallTime{value * 1'000'000};
    }
    [[nodiscard]] constexpr i64 unix_microseconds() const noexcept { return us_; }
    // Giây đầy đủ, làm tròn xuống kể cả với thời điểm trước 1970.
    [[nodiscard]] constexpr i64 unix_seconds() const noexcept {
        return us_ >= 0 ? us_ / 1'000'000 : -((-us_ + 999'999) / 1'000'000);
    }

    friend constexpr Duration operator-(const WallTime a, const WallTime b) noexcept {
        return Duration::microseconds(a.us_ - b.us_);
    }
    friend constexpr WallTime operator+(const WallTime a, const Duration d) noexcept {
        return WallTime{a.us_ + d.as_microseconds()};
    }
    friend constexpr auto operator<=>(WallTime, WallTime) noexcept = default;

private:
    constexpr explicit WallTime(const i64 us) noexcept : us_(us) {}

    i64 us_ = 0;
};

// Ngày dương lịch (proleptic Gregorian) của một số ngày tính từ 1970-01-01.
struct CivilDate {
    i64 year;
    u32 month;  // 1..12
    u32 day;    // 1..31

    friend constexpr bool operator==(const CivilDate&, const CivilDate&) noexcept = default;
};

// Thuật toán civil_from_days của Howard Hinnant (public domain): đúng cho mọi ngày trong ±5,8
// triệu năm, không dùng bảng, không phụ thuộc thư viện lịch của hệ điều hành.
[[nodiscard]] constexpr CivilDate civil_from_days(const i64 days_since_epoch) noexcept {
    const i64 z = days_since_epoch + 719'468;
    const i64 era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = static_cast<u32>(z - era * 146'097);
    const u32 yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u32 mp = (5 * doy + 2) / 153;
    const u32 day = doy - (153 * mp + 2) / 5 + 1;
    const u32 month = mp < 10 ? mp + 3 : mp - 9;
    const i64 year = static_cast<i64>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
    return {year, month, day};
}

// Đồng hồ đơn điệu: không bao giờ lùi, không nhảy theo NTP. Gọi được từ mọi luồng.
class MonotonicClock {
public:
    virtual ~MonotonicClock() = default;
    [[nodiscard]] virtual MonoTime now() const noexcept = 0;

protected:
    MonotonicClock() = default;
    MonotonicClock(const MonotonicClock&) = default;
    MonotonicClock(MonotonicClock&&) = default;
    MonotonicClock& operator=(const MonotonicClock&) = default;
    MonotonicClock& operator=(MonotonicClock&&) = default;
};

// Đồng hồ tường UTC: có thể nhảy khi hệ thống chỉnh giờ. Gọi được từ mọi luồng.
class WallClock {
public:
    virtual ~WallClock() = default;
    [[nodiscard]] virtual WallTime now() const noexcept = 0;

protected:
    WallClock() = default;
    WallClock(const WallClock&) = default;
    WallClock(WallClock&&) = default;
    WallClock& operator=(const WallClock&) = default;
    WallClock& operator=(WallClock&&) = default;
};

// Đồng hồ thật của hệ điều hành. Không trạng thái.
class SystemMonotonicClock final : public MonotonicClock {
public:
    [[nodiscard]] MonoTime now() const noexcept override;
};

class SystemWallClock final : public WallClock {
public:
    [[nodiscard]] WallTime now() const noexcept override;
};

// Đồng hồ giả cho test (X.4): chỉ đổi khi test gọi advance() hay set(). Đọc và ghi từ nhiều luồng
// được: giá trị là một atomic, không có bất biến nào nối nó với dữ liệu khác nên relaxed là đủ.
class FakeMonotonicClock final : public MonotonicClock {
public:
    explicit FakeMonotonicClock(const MonoTime start = {}) noexcept : ns_(start.nanoseconds()) {}

    [[nodiscard]] MonoTime now() const noexcept override {
        return MonoTime::from_nanoseconds(ns_.load(std::memory_order_relaxed));
    }
    void advance(const Duration step) noexcept {
        ns_.fetch_add(step.as_nanoseconds(), std::memory_order_relaxed);
    }
    void set(const MonoTime time) noexcept {
        ns_.store(time.nanoseconds(), std::memory_order_relaxed);
    }

private:
    std::atomic<i64> ns_;
};

class FakeWallClock final : public WallClock {
public:
    explicit FakeWallClock(const WallTime start = {}) noexcept : us_(start.unix_microseconds()) {}

    [[nodiscard]] WallTime now() const noexcept override {
        return WallTime::from_unix_microseconds(us_.load(std::memory_order_relaxed));
    }
    void advance(const Duration step) noexcept {
        us_.fetch_add(step.as_microseconds(), std::memory_order_relaxed);
    }
    void set(const WallTime time) noexcept {
        us_.store(time.unix_microseconds(), std::memory_order_relaxed);
    }

private:
    std::atomic<i64> us_;
};

}  // namespace orion::core

// ISO 8601 UTC với micro giây, ví dụ "2026-09-27T10:20:30.123456Z"; dùng trong log (X.5).
template <>
struct std::formatter<orion::core::WallTime> : std::formatter<std::string_view> {
    template <class FormatContext>
    auto format(const orion::core::WallTime time, FormatContext& ctx) const {
        constexpr orion::i64 kMicrosPerDay = 86'400'000'000;
        const orion::i64 us = time.unix_microseconds();
        const orion::i64 days =
            us >= 0 ? us / kMicrosPerDay : -((-us + kMicrosPerDay - 1) / kMicrosPerDay);
        const orion::i64 in_day = us - days * kMicrosPerDay;
        const orion::core::CivilDate date = orion::core::civil_from_days(days);
        const orion::i64 seconds = in_day / 1'000'000;
        return std::format_to(ctx.out(), "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z", date.year,
                              date.month, date.day, seconds / 3'600, (seconds / 60) % 60,
                              seconds % 60, in_day % 1'000'000);
    }
};
