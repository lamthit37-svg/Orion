#pragma once

// Đếm cấp phát heap cho test và benchmark (CLAUDE.md X.7: hot path không cấp phát sau khi khởi
// động xong; test đếm số lượt cấp phát và chặn nếu khác 0).
//
// alloc_hooks.cpp thay mọi dạng operator new và delete toàn cục của tệp chạy test, và đếm theo
// từng luồng. Chỉ tệp chạy test và benchmark link phần này; sản phẩm không bao giờ link nó.

#include "engine/core/types.hpp"

namespace orion::core::testing {

// Hook có đang đếm không. Dưới TSan thì không: runtime của TSan giữ operator new cho nó
// (engine/core/CMakeLists.txt), và test đếm cấp phát bỏ qua kèm lý do.
[[nodiscard]] constexpr bool allocation_counting_enabled() noexcept {
    return ORION_ALLOC_HOOKS != 0;
}

// Số lần operator new được gọi trên luồng hiện tại, tính từ khi luồng bắt đầu. Luôn 0 khi
// allocation_counting_enabled() là false.
[[nodiscard]] u64 thread_allocation_count() noexcept;

// Đếm cấp phát trên luồng hiện tại trong đời của đối tượng này.
class AllocationScope {
public:
    AllocationScope() noexcept : start_(thread_allocation_count()) {}

    [[nodiscard]] u64 count() const noexcept { return thread_allocation_count() - start_; }

private:
    u64 start_;
};

}  // namespace orion::core::testing
