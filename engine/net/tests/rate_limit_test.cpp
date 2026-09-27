// Giới hạn tần suất (engine/net/rate_limit.hpp; docs/formats/transport.md, mục Giới hạn tần
// suất). Thời gian là đồng hồ giả, khoá hash cố định (X.4).

#include "engine/net/rate_limit.hpp"

#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/short_hash.hpp"
#include "engine/net/address.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace orion::net {
namespace {

using core::Duration;
using core::MonoTime;

constexpr MonoTime kStart = MonoTime::from_nanoseconds(5'000'000'000);
// 10 lần mỗi giây, dồn tới 3.
constexpr RateLimit kLimit = RateLimit::per_second(10, 3);

[[nodiscard]] u32 take_all(TokenBucket& bucket, const RateLimit& limit, const MonoTime now) {
    u32 taken = 0;
    while (taken < 1'000 && bucket.try_take(limit, now)) {
        ++taken;
    }
    return taken;
}

[[nodiscard]] crypto::ShortHashKey fixed_key() {
    EXPECT_TRUE(crypto::initialize().has_value());
    crypto::ShortHashKey key;
    for (usize i = 0; i < crypto::kShortHashKeySize; ++i) {
        key.mutable_view()[i] = static_cast<std::byte>(0xA0 + i);
    }
    return key;
}

TEST(RateLimit, PerSecondAndValidity) {
    EXPECT_EQ(kLimit.interval, Duration::milliseconds(100));
    EXPECT_EQ(kLimit.burst, 3U);
    EXPECT_TRUE(kLimit.valid());
    EXPECT_TRUE(RateLimit::per_second(1'000'000'000, 1).valid());  // 1 ns
    EXPECT_FALSE((RateLimit{.interval = Duration{}, .burst = 1}).valid());
    EXPECT_FALSE((RateLimit{.interval = Duration::seconds(3'601), .burst = 1}).valid());
    EXPECT_FALSE(RateLimit::per_second(10, 0).valid());
    EXPECT_FALSE(RateLimit::per_second(10, RateLimit::kMaxBurst + 1).valid());
    EXPECT_TRUE(RateLimit::per_second(10, RateLimit::kMaxBurst).valid());
}

TEST(TokenBucket, AllowsTheBurstThenOnePerInterval) {
    TokenBucket bucket;
    EXPECT_EQ(take_all(bucket, kLimit, kStart), 3U);
    EXPECT_FALSE(bucket.try_take(kLimit, kStart + Duration::milliseconds(99)));
    EXPECT_TRUE(bucket.try_take(kLimit, kStart + Duration::milliseconds(100)));
    EXPECT_FALSE(bucket.try_take(kLimit, kStart + Duration::milliseconds(150)));
    // Tốc độ đều đúng 10 lần mỗi giây.
    u32 taken = 0;
    for (i64 ms = 200; ms <= 1'100; ms += 10) {
        taken += bucket.try_take(kLimit, kStart + Duration::milliseconds(ms)) ? 1 : 0;
    }
    EXPECT_EQ(taken, 10U);
}

TEST(TokenBucket, IdleTimeRefillsOnlyUpToTheBurst) {
    TokenBucket bucket;
    EXPECT_EQ(take_all(bucket, kLimit, kStart), 3U);
    EXPECT_EQ(take_all(bucket, kLimit, kStart + Duration::seconds(3'600)), 3U);
}

// Đồng hồ lùi (bên gọi sai) không cho thêm lượt nào.
TEST(TokenBucket, TimeGoingBackwardsGrantsNothingExtra) {
    TokenBucket bucket;
    EXPECT_EQ(take_all(bucket, kLimit, kStart), 3U);
    EXPECT_EQ(take_all(bucket, kLimit, kStart + (Duration{} - Duration::seconds(10))), 0U);
    EXPECT_EQ(take_all(bucket, kLimit, kStart + Duration::milliseconds(100)), 1U);
}

TEST(TokenBucket, LargestLimitDoesNotOverflow) {
    const RateLimit largest{.interval = Duration::seconds(3'600), .burst = RateLimit::kMaxBurst};
    ASSERT_TRUE(largest.valid());
    TokenBucket bucket;
    EXPECT_TRUE(bucket.try_take(largest, kStart));
    EXPECT_TRUE(bucket.try_take(largest, kStart));
}

// IPv4 theo địa chỉ, IPv6 theo /64, không theo cổng.
TEST(AddressRateLimiter, SourcesAreAddressesOrSlash64sNotPorts) {
    AddressRateLimiter limiter(RateLimit::per_second(1, 2), 1'024, fixed_key());
    const Address a1 = Address::v4({192, 0, 2, 10}, 1'000);
    const Address a2 = Address::v4({192, 0, 2, 10}, 2'000);
    const Address b = Address::v4({198, 51, 100, 7}, 1'000);
    EXPECT_TRUE(limiter.try_take(a1, kStart));
    EXPECT_TRUE(limiter.try_take(a2, kStart));
    EXPECT_FALSE(limiter.try_take(a1, kStart));
    EXPECT_TRUE(limiter.try_take(b, kStart));

    std::array<u8, 16> v6{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1};
    const Address host1 = Address::v6(v6, 1);
    v6[15] = 9;
    const Address host2 = Address::v6(v6, 2);  // Cùng /64.
    v6[7] = 2;
    const Address other = Address::v6(v6, 3);  // /64 khác.
    EXPECT_TRUE(limiter.try_take(host1, kStart));
    EXPECT_TRUE(limiter.try_take(host2, kStart));
    EXPECT_FALSE(limiter.try_take(host1, kStart));
    EXPECT_TRUE(limiter.try_take(other, kStart));
    EXPECT_TRUE(limiter.try_take(host2, kStart + Duration::seconds(1)));
}

// Một ô: mọi nguồn dùng chung bucket (bảng không bao giờ đầy, nguồn trùng ô chia nhau hạn mức).
TEST(AddressRateLimiter, SourcesInOneSlotShareTheBucket) {
    AddressRateLimiter limiter(RateLimit::per_second(1, 2), 1, fixed_key());
    EXPECT_TRUE(limiter.try_take(Address::v4({192, 0, 2, 1}, 1), kStart));
    EXPECT_TRUE(limiter.try_take(Address::v4({192, 0, 2, 2}, 1), kStart));
    EXPECT_FALSE(limiter.try_take(Address::v4({192, 0, 2, 3}, 1), kStart));
}

// X.7: lấy lượt không cấp phát.
TEST(AddressRateLimiter, TakingDoesNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    AddressRateLimiter limiter(kLimit, 64, fixed_key());
    TokenBucket bucket;
    const core::testing::AllocationScope scope;
    for (u8 i = 0; i < 200; ++i) {
        static_cast<void>(limiter.try_take(Address::v4({10, 0, 0, i}, 1), kStart));
        static_cast<void>(bucket.try_take(kLimit, kStart));
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::net
