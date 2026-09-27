#include "engine/core/queue.hpp"

#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// Test của T0 không dùng được engine/jobs (T1), nên tạo std::thread trực tiếp; các test nhiều luồng
// ở đây là thứ preset linux-tsan kiểm race (X.7).

namespace orion::core {
namespace {

TEST(SpscRing, FifoWithFullAndEmpty) {
    SpscRing<i32> ring{3};
    EXPECT_EQ(ring.capacity(), 4U) << "làm tròn lên lũy thừa của 2";
    for (i32 i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.try_push(i));
    }
    EXPECT_FALSE(ring.try_push(99)) << "đầy";
    i32 out = -1;
    for (i32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(ring.try_pop(out)) << "rỗng";
}

TEST(SpscRing, MoveOnlyValues) {
    SpscRing<std::unique_ptr<i32>> ring{2};
    EXPECT_TRUE(ring.try_push(std::make_unique<i32>(7)));
    std::unique_ptr<i32> out;
    ASSERT_TRUE(ring.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
}

TEST(SpscRing, PushPopNeverAllocate) {
    SpscRing<u64> ring{64};
    const testing::AllocationScope scope;
    u64 out = 0;
    for (u64 i = 0; i < 1'000; ++i) {
        EXPECT_TRUE(ring.try_push(i));
        EXPECT_TRUE(ring.try_pop(out));
    }
    EXPECT_EQ(scope.count(), 0U);
}

TEST(SpscRing, TwoThreadsKeepOrder) {
    constexpr u64 kCount = 200'000;
    SpscRing<u64> ring{256};
    std::thread producer([&ring] {
        for (u64 i = 0; i < kCount;) {
            if (ring.try_push(i)) {
                ++i;
            }
        }
    });
    u64 expected = 0;
    u64 out = 0;
    while (expected < kCount) {
        if (ring.try_pop(out)) {
            ASSERT_EQ(out, expected);
            ++expected;
        }
    }
    producer.join();
}

TEST(MpmcRing, FifoWithFullAndEmpty) {
    MpmcRing<i32> ring{4};
    for (i32 i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.try_push(i));
    }
    EXPECT_FALSE(ring.try_push(99));
    i32 out = -1;
    for (i32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(ring.try_pop(out));
    // Quay vòng nhiều lượt để chạm số thứ tự lớn hơn sức chứa.
    for (i32 round = 0; round < 100; ++round) {
        EXPECT_TRUE(ring.try_push(round));
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, round);
    }
}

TEST(MpmcRing, ManyProducersManyConsumersDeliverEachItemOnce) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr u64 kPerProducer = 50'000;
    MpmcRing<u64> ring{1024};
    std::vector<std::atomic<u32>> seen(kProducers * kPerProducer);
    std::atomic<u64> consumed{0};
    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&ring, p] {
            const u64 base = static_cast<u64>(p) * kPerProducer;
            for (u64 i = 0; i < kPerProducer;) {
                if (ring.try_push(base + i)) {
                    ++i;
                }
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&ring, &seen, &consumed] {
            u64 item = 0;
            while (consumed.load() < kProducers * kPerProducer) {
                if (ring.try_pop(item)) {
                    seen[item].fetch_add(1);
                    consumed.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (usize i = 0; i < seen.size(); ++i) {
        ASSERT_EQ(seen[i].load(), 1U) << "phần tử " << i;
    }
}

}  // namespace
}  // namespace orion::core
