#include "engine/core/slot_map.hpp"

#include "engine/core/error.hpp"
#include "engine/core/handle.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <vector>

namespace orion::core {
namespace {

struct MeshTag {};
struct TextureTag {};

static_assert(!std::is_convertible_v<Handle<MeshTag>, Handle<TextureTag>>,
              "handle khác tag không được gán cho nhau");
static_assert(sizeof(Handle<MeshTag>) == sizeof(u64));
static_assert(!Handle<MeshTag>{}.is_valid());
static_assert(Handle<MeshTag>::from_bits(Handle<MeshTag>::from_parts(7, 3).bits()) ==
              Handle<MeshTag>::from_parts(7, 3));
static_assert(Handle<MeshTag>::from_parts(7, 3).bits() == 0x0000'0003'0000'0007ULL);

using IntMap = SlotMap<i32>;

IntMap::HandleType must_insert(IntMap& map, const i32 value) {
    auto handle = map.insert(value);
    EXPECT_TRUE(handle.has_value());
    return handle.has_value() ? *handle : IntMap::HandleType{};
}

TEST(SlotMap, InsertGetRemove) {
    IntMap map{4};
    const auto a = must_insert(map, 10);
    const auto b = must_insert(map, 20);
    ASSERT_NE(map.get(a), nullptr);
    EXPECT_EQ(*map.get(a), 10);
    EXPECT_EQ(*map.get(b), 20);
    EXPECT_EQ(map.size(), 2U);
    EXPECT_TRUE(map.remove(a));
    EXPECT_FALSE(map.contains(a));
    EXPECT_EQ(map.get(a), nullptr);
    EXPECT_FALSE(map.remove(a)) << "xoá hai lần phải báo false";
    EXPECT_EQ(*map.get(b), 20) << "handle khác vẫn đúng sau khi đổi chỗ";
}

TEST(SlotMap, StaleHandleNeverMatchesReusedSlot) {
    IntMap map{1};
    const auto first = must_insert(map, 1);
    EXPECT_TRUE(map.remove(first));
    const auto second = must_insert(map, 2);
    EXPECT_EQ(second.index(), first.index());
    EXPECT_EQ(second.generation(), first.generation() + 1);
    EXPECT_EQ(map.get(first), nullptr);
    EXPECT_EQ(*map.get(second), 2);
}

TEST(SlotMap, ValuesStayDenseAndHandleAtMapsBack) {
    IntMap map{8};
    std::vector<IntMap::HandleType> handles;
    handles.reserve(6);
    for (i32 i = 0; i < 6; ++i) {
        handles.push_back(must_insert(map, i * 10));
    }
    EXPECT_TRUE(map.remove(handles[1]));
    EXPECT_TRUE(map.remove(handles[4]));
    std::vector<i32> seen(map.values().begin(), map.values().end());
    std::ranges::sort(seen);
    EXPECT_EQ(seen, (std::vector<i32>{0, 20, 30, 50}));
    for (u32 position = 0; position < map.size(); ++position) {
        const auto handle = map.handle_at(position);
        ASSERT_NE(map.get(handle), nullptr);
        EXPECT_EQ(*map.get(handle), map.values()[position]);
    }
}

TEST(SlotMap, FullMapReportsResourceExhausted) {
    IntMap map{2};
    static_cast<void>(must_insert(map, 1));
    static_cast<void>(must_insert(map, 2));
    const auto third = map.insert(3);
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().code(), ErrorCode::ResourceExhausted);
}

TEST(SlotMap, SlotIsRetiredInsteadOfWrappingGeneration) {
    SlotMap<i32, i32, 2> map{1};
    auto handle = map.insert(1);
    ASSERT_TRUE(handle.has_value());
    EXPECT_TRUE(map.remove(*handle));
    handle = map.insert(2);
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ(handle->generation(), 2U);
    EXPECT_TRUE(map.remove(*handle));
    EXPECT_EQ(map.retired(), 1U);
    const auto refused = map.insert(3);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::ResourceExhausted);
}

TEST(SlotMap, ForgedHandlesAreRejected) {
    IntMap map{2};
    const auto live = must_insert(map, 5);
    EXPECT_EQ(map.get(IntMap::HandleType::from_parts(99, 1)), nullptr);
    EXPECT_EQ(map.get(IntMap::HandleType::from_parts(live.index(), 7)), nullptr);
    // Ô 1 chưa từng cấp: generation khớp giá trị mặc định nhưng ô không có giá trị.
    EXPECT_EQ(map.get(IntMap::HandleType::from_parts(1, 1)), nullptr);
    EXPECT_EQ(map.get(IntMap::HandleType{}), nullptr);
}

TEST(SlotMap, MoveOnlyValues) {
    SlotMap<std::unique_ptr<i32>> map{2};
    auto handle = map.insert(std::make_unique<i32>(42));
    ASSERT_TRUE(handle.has_value());
    auto* stored = map.get(*handle);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(**stored, 42);
    EXPECT_TRUE(map.remove(*handle));
}

TEST(SlotMap, InsertAndRemoveNeverAllocateAfterConstruction) {
    IntMap map{64};
    std::vector<IntMap::HandleType> handles;
    handles.reserve(64);
    const testing::AllocationScope scope;
    for (i32 round = 0; round < 3; ++round) {
        for (i32 i = 0; i < 64; ++i) {
            auto handle = map.insert(i);
            if (handle.has_value()) {
                handles.push_back(*handle);
            }
        }
        for (const auto handle : handles) {
            static_cast<void>(map.remove(handle));
        }
        handles.clear();
    }
    EXPECT_EQ(scope.count(), 0U);
    EXPECT_TRUE(map.empty());
}

}  // namespace
}  // namespace orion::core
