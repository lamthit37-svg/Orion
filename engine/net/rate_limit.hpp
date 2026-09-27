#pragma once

// Giới hạn tần suất (CLAUDE.md X.9: mọi endpoint có giới hạn tần suất; docs/formats/transport.md,
// mục Giới hạn tần suất) kiểu token bucket, viết theo GCRA: bucket chỉ là một mốc "sẵn sàng", mỗi
// lần lấy đẩy mốc thêm `interval`, và lần lấy chỉ qua khi mốc mới không vượt `now + burst ×
// interval`. Tính bằng số nguyên nano giây nên không trôi, và nhận thời gian từ bên gọi (X.4).
// try_take không cấp phát; AddressRateLimiter cấp bảng một lần lúc tạo.

#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/short_hash.hpp"
#include "engine/net/address.hpp"

#include <cstddef>
#include <vector>

namespace orion::net {

struct RateLimit {
    // Thời gian giữa hai lần lấy ở tốc độ đều, 1 ns tới 1 giờ.
    core::Duration interval;
    // Số lần lấy liền nhau khi bucket đầy, 1 tới kMaxBurst.
    u32 burst = 1;

    static constexpr u32 kMaxBurst = 1'000'000;

    // `rate` lần mỗi giây (1 tới 1e9), dồn tới `burst` lần.
    [[nodiscard]] static constexpr RateLimit per_second(const u32 rate, const u32 burst) noexcept {
        return {.interval = core::Duration::nanoseconds(1'000'000'000 / (rate == 0 ? 1 : rate)),
                .burst = burst};
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return interval > core::Duration{} && interval <= core::Duration::seconds(3'600) &&
               burst >= 1 && burst <= kMaxBurst;
    }
};

class TokenBucket {
public:
    // Lấy một lượt: true khi còn, false (không đổi gì) khi đã hết. limit.valid().
    [[nodiscard]] bool try_take(const RateLimit& limit, core::MonoTime now) noexcept;

private:
    // Mốc mà bucket đầy lại nếu không lấy thêm; quá khứ nghĩa là đang đầy.
    core::MonoTime ready_;
};

// Một bucket cho mỗi nguồn: IPv4 theo cả địa chỉ, IPv6 theo /64 (một khách hàng thường có cả một
// /64), không theo cổng. Bảng cỡ cố định; nguồn vào ô theo SipHash với khoá bí mật, nên bên ngoài
// không dựng được các nguồn trùng ô. Các nguồn trùng ô dùng chung bucket: bảng không bao giờ đầy,
// đổi lại chúng chia nhau hạn mức.
class AddressRateLimiter {
public:
    // limit.valid(), slots >= 1. Server dùng khoá crypto::generate_short_hash_key(); test đưa khoá
    // cố định để kết quả tất định (X.4).
    AddressRateLimiter(RateLimit limit, usize slots, crypto::ShortHashKey key);

    [[nodiscard]] bool try_take(const Address& from, core::MonoTime now) noexcept;

private:
    RateLimit limit_;
    crypto::ShortHashKey key_;
    std::vector<TokenBucket> buckets_;
};

}  // namespace orion::net
