#pragma once

// Tham chiếu có generation qua ranh giới giữa các hệ thống (CLAUDE.md X.7, ADR 0006).
//
// Handle gồm 32 bit chỉ số và 32 bit generation. Kho cấp handle (ví dụ SlotMap) tăng generation của
// một ô mỗi lần ô đó được tái dùng, nên handle cũ không bao giờ khớp đối tượng mới. Generation 0
// nghĩa là handle rỗng. Tag chỉ để phân biệt kiểu: Handle<Mesh> không gán được cho
// Handle<Texture>.
//
// Handle chỉ sống trong một tiến trình: không ghi xuống đĩa, không gửi qua mạng (ADR 0006).

#include "engine/core/types.hpp"

#include <compare>

namespace orion {

template <class Tag>
class [[nodiscard]] Handle {
public:
    constexpr Handle() noexcept = default;

    [[nodiscard]] static constexpr Handle from_parts(const u32 index,
                                                     const u32 generation) noexcept {
        Handle handle;
        handle.index_ = index;
        handle.generation_ = generation;
        return handle;
    }

    // Ngược của bits(); dùng khi handle phải đi qua API chỉ nhận số nguyên (ví dụ user data).
    [[nodiscard]] static constexpr Handle from_bits(const u64 bits) noexcept {
        return from_parts(static_cast<u32>(bits & 0xFFFF'FFFFU), static_cast<u32>(bits >> 32U));
    }

    [[nodiscard]] constexpr u32 index() const noexcept { return index_; }
    [[nodiscard]] constexpr u32 generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr u64 bits() const noexcept {
        return (u64{generation_} << 32U) | u64{index_};
    }
    // Handle không rỗng. Không có nghĩa là đối tượng còn sống: hỏi kho đã cấp nó.
    [[nodiscard]] constexpr bool is_valid() const noexcept { return generation_ != 0; }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;
    friend constexpr auto operator<=>(Handle, Handle) noexcept = default;

private:
    u32 index_ = 0;
    u32 generation_ = 0;
};

}  // namespace orion
