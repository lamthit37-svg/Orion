#include "engine/core/arena.hpp"

#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace orion::core {
namespace {

bool is_aligned(const void* pointer, const usize alignment) {
    void* candidate = const_cast<void*>(pointer);  // NOLINT(*-const-cast): std::align chỉ đọc
    usize space = alignment;
    return std::align(alignment, 1, candidate, space) == pointer;
}

struct Point {
    i32 x = 0;
    i32 y = 0;
};

// Compiler được phép bỏ hẳn cặp new/delete mà con trỏ không thoát ra ngoài (C++ [expr.new]), và
// clang làm vậy ở -O2. Ghi con trỏ vào biến volatile để phép cấp phát thật sự xảy ra khi đo.
const void* escape(const void* pointer) {
    // NOLINTNEXTLINE(*-avoid-non-const-global-variables): đích ghi của rào tối ưu trong test.
    static const void* volatile sink = nullptr;
    sink = pointer;
    return sink;
}

TEST(Arena, AllocatesAlignedBlocksUntilFull) {
    alignas(64) std::array<std::byte, 256> storage{};
    Arena arena{storage};
    void* a = arena.allocate(3, 1);
    void* b = arena.allocate(8, 16);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(is_aligned(b, 16));
    EXPECT_EQ(arena.used(), 24U);  // 3 byte, đệm tới 16, rồi 8 byte.
    EXPECT_EQ(arena.allocate(1024, 1), nullptr);
    EXPECT_EQ(arena.used(), 24U) << "cấp phát hỏng không được đổi trạng thái";
}

TEST(Arena, CreateAndArraysValueInitialize) {
    alignas(16) std::array<std::byte, 128> storage{};
    storage.fill(std::byte{0xCD});
    Arena arena{storage};
    const Point* point = arena.create<Point>(Point{3, 4});
    ASSERT_NE(point, nullptr);
    EXPECT_EQ(point->x, 3);
    EXPECT_EQ(point->y, 4);
    const std::span<u32> values = arena.allocate_array<u32>(8);
    ASSERT_EQ(values.size(), 8U);
    for (const u32 value : values) {
        EXPECT_EQ(value, 0U);
    }
    EXPECT_TRUE(arena.allocate_array<u64>(1'000'000).empty());
}

TEST(Arena, MarkRewindAndResetTrackHighWater) {
    std::array<std::byte, 64> storage{};
    Arena arena{storage};
    static_cast<void>(arena.allocate(8, 1));
    const Arena::Marker marker = arena.mark();
    static_cast<void>(arena.allocate(40, 1));
    EXPECT_EQ(arena.used(), 48U);
    arena.rewind(marker);
    EXPECT_EQ(arena.used(), 8U);
    arena.reset();
    EXPECT_EQ(arena.used(), 0U);
    EXPECT_EQ(arena.high_water(), 48U);
    EXPECT_EQ(arena.capacity(), 64U);
}

TEST(Arena, NeverTouchesTheHeap) {
    std::array<std::byte, 512> storage{};
    Arena arena{storage};
    const testing::AllocationScope scope;
    for (int round = 0; round < 10; ++round) {
        static_cast<void>(arena.create<Point>());
        static_cast<void>(arena.allocate_array<u16>(16));
        arena.reset();
    }
    EXPECT_EQ(scope.count(), 0U);
}

TEST(AllocationScope, CountsHeapAllocationsOnThisThread) {
    if (!testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    const testing::AllocationScope scope;
    auto first = std::make_unique<Point>();
    escape(first.get());
    auto second = std::make_unique<std::array<std::byte, 4096>>();
    escape(second.get());
    EXPECT_EQ(scope.count(), 2U);
}

TEST(AllocationScope, OverAlignedAllocationsWork) {
    if (!testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    struct alignas(256) Wide {
        std::array<std::byte, 256> bytes{};
    };
    const testing::AllocationScope scope;
    const auto wide = std::make_unique<Wide>();
    escape(wide.get());
    EXPECT_TRUE(is_aligned(wide.get(), 256));
    EXPECT_EQ(scope.count(), 1U);
}

}  // namespace
}  // namespace orion::core
