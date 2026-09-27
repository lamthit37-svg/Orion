#pragma once

// Kho đối tượng truy cập bằng Handle có generation, lưu liền trong một mảng (CLAUDE.md X.7, X.8).
//
// - Mọi bộ nhớ được cấp lúc dựng theo sức chứa cố định; insert và remove không bao giờ cấp phát
//   heap, nên dùng được trên hot path sau khi khởi động xong.
// - Giá trị nằm liền nhau trong values(), duyệt tuần tự; remove đổi chỗ phần tử cuối vào lỗ trống,
//   nên thứ tự duyệt chỉ phụ thuộc chuỗi thao tác, không phụ thuộc địa chỉ (X.11).
// - Handle của đối tượng đã xoá không bao giờ trỏ nhầm sang đối tượng mới: generation của ô tăng
//   mỗi lần xoá. Ô chạm MaxGeneration thì bị loại vĩnh viễn thay vì quay vòng về 0.
//
// Độ phức tạp: insert, remove, get đều O(1). Luồng: không đồng bộ; mỗi SlotMap thuộc một luồng.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/handle.hpp"
#include "engine/core/types.hpp"

#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace orion::core {

template <class T, class Tag = T, u32 MaxGeneration = std::numeric_limits<u32>::max()>
class SlotMap {
public:
    using HandleType = Handle<Tag>;

    static_assert(MaxGeneration >= 1);

    explicit SlotMap(const u32 capacity) : slots_(capacity) {
        dense_.reserve(capacity);
        dense_to_slot_.reserve(capacity);
    }

    // Thêm một giá trị. ResourceExhausted khi đầy, hoặc khi mọi ô còn lại đã bị loại.
    [[nodiscard]] Result<HandleType> insert(T value) {
        if (dense_.size() == slots_.size()) {
            return fail(ErrorCode::ResourceExhausted, "SlotMap: hết sức chứa");
        }
        u32 index = 0;
        if (free_head_ != kNone) {
            index = free_head_;
            free_head_ = slots_[index].link;
        } else if (next_unused_ < slots_.size()) {
            index = next_unused_++;
        } else {
            return fail(ErrorCode::ResourceExhausted, "SlotMap: mọi ô trống đã bị loại");
        }
        Slot& slot = slots_[index];
        slot.link = static_cast<u32>(dense_.size());
        slot.occupied = true;
        dense_.push_back(std::move(value));
        dense_to_slot_.push_back(index);
        return HandleType::from_parts(index, slot.generation);
    }

    // Con trỏ tới giá trị, hoặc nullptr khi handle rỗng, cũ, hay không do kho này cấp. Con trỏ chỉ
    // sống tới lần insert hoặc remove kế tiếp (X.7).
    [[nodiscard]] T* get(const HandleType handle) noexcept {
        const Slot* slot = live_slot(handle);
        return slot == nullptr ? nullptr : &dense_[slot->link];
    }
    [[nodiscard]] const T* get(const HandleType handle) const noexcept {
        const Slot* slot = live_slot(handle);
        return slot == nullptr ? nullptr : &dense_[slot->link];
    }
    [[nodiscard]] bool contains(const HandleType handle) const noexcept {
        return live_slot(handle) != nullptr;
    }

    // Xoá giá trị; false nếu handle không còn sống. Handle của các giá trị khác vẫn đúng.
    bool remove(const HandleType handle) noexcept {
        if (live_slot(handle) == nullptr) {
            return false;
        }
        Slot& slot = slots_[handle.index()];
        const u32 hole = slot.link;
        const auto last = static_cast<u32>(dense_.size() - 1);
        if (hole != last) {
            dense_[hole] = std::move(dense_[last]);
            dense_to_slot_[hole] = dense_to_slot_[last];
            slots_[dense_to_slot_[hole]].link = hole;
        }
        dense_.pop_back();
        dense_to_slot_.pop_back();
        slot.occupied = false;
        if (slot.generation == MaxGeneration) {
            ++retired_;  // Không quay vòng generation: ô này không bao giờ được cấp lại.
        } else {
            ++slot.generation;
            slot.link = free_head_;
            free_head_ = handle.index();
        }
        return true;
    }

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(dense_.size()); }
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(slots_.size()); }
    [[nodiscard]] u32 retired() const noexcept { return retired_; }
    [[nodiscard]] bool empty() const noexcept { return dense_.empty(); }

    // Mọi giá trị đang sống, liền nhau. Thứ tự đổi khi remove.
    [[nodiscard]] std::span<T> values() noexcept { return dense_; }
    [[nodiscard]] std::span<const T> values() const noexcept { return dense_; }

    // Handle của values()[position]. Tiền điều kiện: position < size().
    [[nodiscard]] HandleType handle_at(const u32 position) const noexcept {
        ORION_ASSERT(position < dense_.size(), "vị trí {} ngoài [0, {})", position, dense_.size());
        const u32 index = dense_to_slot_[position];
        return HandleType::from_parts(index, slots_[index].generation);
    }

private:
    static constexpr u32 kNone = std::numeric_limits<u32>::max();

    struct Slot {
        u32 generation = 1;
        // Khi ô có giá trị: vị trí trong dense_. Khi ô trống: ô trống kế tiếp trong danh sách.
        u32 link = kNone;
        bool occupied = false;
    };

    [[nodiscard]] const Slot* live_slot(const HandleType handle) const noexcept {
        if (handle.index() >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index()];
        if (!slot.occupied || slot.generation != handle.generation()) {
            return nullptr;
        }
        return &slot;
    }

    std::vector<Slot> slots_;
    std::vector<T> dense_;
    std::vector<u32> dense_to_slot_;
    u32 free_head_ = kNone;
    u32 next_unused_ = 0;
    u32 retired_ = 0;
};

}  // namespace orion::core
