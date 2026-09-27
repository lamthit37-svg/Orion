#include "engine/core/arena.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"

#include <algorithm>
#include <bit>
#include <memory>

namespace orion::core {

void* Arena::allocate(const usize size, const usize alignment) noexcept {
    ORION_ASSERT(std::has_single_bit(alignment), "alignment {} không phải lũy thừa của 2",
                 alignment);
    void* cursor = memory_.data() + offset_;
    usize space = memory_.size() - offset_;
    void* result = std::align(alignment, size, cursor, space);
    if (result == nullptr) {
        return nullptr;
    }
    // std::align đã chỉnh cursor và space theo phần đệm căn lề; phần còn lại sau khối là
    // space-size.
    offset_ = memory_.size() - (space - size);
    high_water_ = std::max(high_water_, offset_);
    return result;
}

void Arena::rewind(const Marker marker) noexcept {
    ORION_ASSERT(marker.offset <= offset_, "marker {} nằm sau vị trí hiện tại {}", marker.offset,
                 offset_);
    offset_ = marker.offset;
}

}  // namespace orion::core
