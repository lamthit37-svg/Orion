#include "engine/net/rate_limit.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/short_hash.hpp"
#include "engine/net/address.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace orion::net {

bool TokenBucket::try_take(const RateLimit& limit, const core::MonoTime now) noexcept {
    ORION_ASSERT(limit.valid(), "giới hạn tần suất sai");
    // burst × interval tối đa 1e6 giờ tính bằng nano giây: vừa i64.
    const core::MonoTime next = std::max(ready_, now) + limit.interval;
    if (next - now > limit.interval * limit.burst) {
        return false;
    }
    ready_ = next;
    return true;
}

AddressRateLimiter::AddressRateLimiter(const RateLimit limit, const usize slots,
                                       crypto::ShortHashKey key)
    : limit_(limit), key_(std::move(key)), buckets_(slots) {
    ORION_VERIFY(limit.valid() && slots >= 1, "bộ giới hạn theo địa chỉ cấu hình sai");
}

bool AddressRateLimiter::try_take(const Address& from, const core::MonoTime now) noexcept {
    // Họ địa chỉ rồi các byte của nguồn: IPv4 4 byte, IPv6 8 byte đầu (/64).
    std::array<std::byte, 9> source{};
    source[0] = static_cast<std::byte>(std::to_underlying(from.family()));
    const std::span<const u8> bytes = from.bytes();
    const usize length = from.family() == AddressFamily::V4 ? 4 : 8;
    for (usize i = 0; i < length; ++i) {
        source[1 + i] = static_cast<std::byte>(bytes[i]);
    }
    const u64 hash = crypto::short_hash(std::span(source).first(1 + length), key_);
    return buckets_[hash % buckets_.size()].try_take(limit_, now);
}

}  // namespace orion::net
