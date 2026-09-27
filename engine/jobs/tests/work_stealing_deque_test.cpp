#include "engine/jobs/detail/work_stealing_deque.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/jobs/thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <utility>
#include <vector>

namespace orion::jobs::detail {
namespace {

// Phần tử 32 byte như Job; hai từ dư mang bản sao để phát hiện ô bị đọc lẫn giữa hai lần ghi.
struct Item {
    u64 value = 0;
    u64 inverted = 0;
    u64 doubled = 0;
    u64 padding = 0;
};

[[nodiscard]] Item make_item(const u64 value) {
    return {value, ~value, value * 2, 0};
}

[[nodiscard]] bool intact(const Item& item) {
    return item.inverted == ~item.value && item.doubled == item.value * 2;
}

constexpr u64 kNothing = ~u64{0};

// Giá trị lấy được, hoặc kNothing khi deque trả rỗng.
[[nodiscard]] u64 value_of(const std::optional<Item>& item) {
    return item.has_value() ? item->value : kNothing;
}

TEST(WorkStealingDeque, OwnerPopsNewestAndThiefStealsOldest) {
    WorkStealingDeque<Item> deque(8);
    for (u64 i = 1; i <= 4; ++i) {
        ASSERT_TRUE(deque.push(make_item(i)));
    }
    EXPECT_EQ(value_of(deque.pop()), 4U);
    EXPECT_EQ(value_of(deque.steal()), 1U);
    EXPECT_EQ(value_of(deque.pop()), 3U);
    EXPECT_EQ(value_of(deque.steal()), 2U);
    EXPECT_FALSE(deque.pop().has_value());
    EXPECT_FALSE(deque.steal().has_value());
}

TEST(WorkStealingDeque, CapacityRoundsUpAndPushFailsWhenFull) {
    WorkStealingDeque<Item> deque(5);
    EXPECT_EQ(deque.capacity(), 8U);
    for (u64 i = 0; i < 8; ++i) {
        ASSERT_TRUE(deque.push(make_item(i)));
    }
    EXPECT_FALSE(deque.push(make_item(99)));
    EXPECT_EQ(value_of(deque.steal()), 0U);
    EXPECT_TRUE(deque.push(make_item(8)));
    EXPECT_EQ(WorkStealingDeque<Item>(0).capacity(), 2U);
}

TEST(WorkStealingDeque, IndicesWrapAroundManyTimes) {
    WorkStealingDeque<Item> deque(4);
    u64 next = 0;
    u64 expected_steal = 0;
    for (u32 round = 0; round < 1'000; ++round) {
        ASSERT_TRUE(deque.push(make_item(next++)));
        ASSERT_TRUE(deque.push(make_item(next++)));
        ASSERT_EQ(value_of(deque.steal()), expected_steal);
        ASSERT_EQ(value_of(deque.pop()), next - 1);
        expected_steal = next;
    }
    EXPECT_FALSE(deque.pop().has_value());
}

// Luồng chủ đẩy và lấy xen kẽ trong khi ba luồng trộm liên tục; mọi phần tử phải được lấy đúng một
// lần và nguyên vẹn. Chạy dưới TSan ở preset linux-tsan (X.7).
TEST(WorkStealingDeque, EveryItemIsTakenExactlyOnceUnderContention) {
    constexpr u64 kItems = 200'000;
    WorkStealingDeque<Item> deque(64);
    std::vector<std::atomic<u8>> taken(kItems);
    std::atomic<bool> producing{true};
    std::atomic<u64> corrupted{0};
    auto record = [&](const Item& item) {
        if (!intact(item) || item.value >= kItems) {
            corrupted.fetch_add(1);
            return;
        }
        taken[item.value].fetch_add(1);
    };

    std::vector<Thread> thieves;
    for (u32 i = 0; i < 3; ++i) {
        Result<Thread> thief = Thread::start("thief", [&](StopToken /*token*/) {
            while (producing.load(std::memory_order_acquire)) {
                if (const std::optional<Item> item = deque.steal()) {
                    record(*item);
                }
            }
        });
        ASSERT_TRUE(thief.has_value());
        thieves.push_back(std::move(*thief));
    }

    for (u64 next = 0; next < kItems;) {
        if (deque.push(make_item(next))) {
            ++next;
        }
        if (next % 3 == 0) {
            if (const std::optional<Item> item = deque.pop()) {
                record(*item);
            }
        }
    }
    while (const std::optional<Item> item = deque.pop()) {
        record(*item);
    }
    producing.store(false, std::memory_order_release);
    thieves.clear();

    EXPECT_EQ(corrupted.load(), 0U);
    u64 missing = 0;
    u64 duplicated = 0;
    for (const std::atomic<u8>& count : taken) {
        missing += count.load() == 0 ? 1 : 0;
        duplicated += count.load() > 1 ? 1 : 0;
    }
    EXPECT_EQ(missing, 0U);
    EXPECT_EQ(duplicated, 0U);
}

}  // namespace
}  // namespace orion::jobs::detail
