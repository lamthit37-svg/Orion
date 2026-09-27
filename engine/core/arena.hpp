#pragma once

// Bộ cấp phát tuyến tính trên một vùng nhớ cố định (CLAUDE.md X.7).
//
// Cấp phát là cộng một con trỏ; giải phóng là reset() cả vùng một lần. Dùng cho dữ liệu sống trong
// một frame hay một tick, để hot path không cấp phát heap. Arena không sở hữu vùng nhớ: người gọi
// cấp nó lúc khởi động và giữ nó sống lâu hơn arena.
//
// Luồng: không đồng bộ. Mỗi arena thuộc đúng một luồng tại một thời điểm.

#include "engine/core/types.hpp"

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace orion::core {

class Arena {
public:
    // Vị trí đã dùng, để quay lại sau một phạm vi tạm (rewind).
    struct Marker {
        usize offset;
    };

    explicit Arena(std::span<std::byte> memory) noexcept : memory_(memory) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;
    ~Arena() = default;

    // Vùng nhớ `size` byte căn theo `alignment` (lũy thừa của 2), hoặc nullptr khi hết chỗ. O(1).
    [[nodiscard]] void* allocate(usize size, usize alignment) noexcept;

    // Dựng một T trong arena. Arena không bao giờ gọi destructor, nên T phải huỷ tầm thường.
    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "arena không gọi destructor");
        void* memory = allocate(sizeof(T), alignof(T));
        if (memory == nullptr) {
            return nullptr;
        }
        return std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
    }

    // Mảng `count` phần tử T khởi tạo giá trị (về 0 với kiểu số), hoặc span rỗng khi hết chỗ.
    template <class T>
    [[nodiscard]] std::span<T> allocate_array(const usize count) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "arena không gọi destructor");
        static_assert(std::is_default_constructible_v<T>);
        if (count > memory_.size() / sizeof(T)) {
            return {};
        }
        void* memory = allocate(count * sizeof(T), alignof(T));
        if (memory == nullptr) {
            return {};
        }
        // Dựng từng phần tử thay vì placement new[], để không phụ thuộc chuyện compiler có thêm
        // phần đầu mảng hay không.
        T* first = static_cast<T*>(memory);
        std::uninitialized_value_construct_n(first, count);
        return {first, count};
    }

    [[nodiscard]] Marker mark() const noexcept { return Marker{offset_}; }
    // Trả mọi cấp phát sau `marker`. Tiền điều kiện: marker lấy từ chính arena này, sau lần reset
    // gần nhất.
    void rewind(Marker marker) noexcept;
    void reset() noexcept { offset_ = 0; }

    [[nodiscard]] usize used() const noexcept { return offset_; }
    [[nodiscard]] usize capacity() const noexcept { return memory_.size(); }
    // Mức dùng cao nhất từ khi dựng, để đo ngân sách bộ nhớ của frame hay tick (X.8).
    [[nodiscard]] usize high_water() const noexcept { return high_water_; }

private:
    std::span<std::byte> memory_;
    usize offset_ = 0;
    usize high_water_ = 0;
};

}  // namespace orion::core
