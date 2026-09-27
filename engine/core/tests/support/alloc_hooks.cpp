// Thay operator new và delete toàn cục để đếm cấp phát (xem alloc_hooks.hpp).
//
// Đây là nơi duy nhất trong repo gọi malloc và free trần ngoài allocator của engine/core, và chỉ
// được link vào tệp chạy test: operator new thay thế buộc phải tự lấy bộ nhớ từ C runtime.
// Cấp phát có căn lề tự làm bằng cách cấp dư rồi căn, vì std::aligned_alloc không có trên MSVC.

#include "engine/core/tests/support/alloc_hooks.hpp"

#include "engine/core/types.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace orion::core::testing {
namespace {

// Mỗi luồng một bộ đếm, không cần đồng bộ: chỉ luồng sở hữu đọc và ghi nó.
// NOLINTNEXTLINE(*-avoid-non-const-global-variables): trạng thái của hook test, không của sản phẩm.
thread_local u64 g_thread_allocations = 0;

#if ORION_ALLOC_HOOKS

// Đầu mỗi khối căn lề lưu con trỏ gốc do malloc trả, để delete trả đúng khối cho free.
constexpr usize kHeader = sizeof(void*);

void* allocate(const usize size, const usize alignment) noexcept {
    ++g_thread_allocations;
    const usize request = size == 0 ? 1 : size;
    if (alignment <= alignof(std::max_align_t)) {
        // NOLINTNEXTLINE(*-no-malloc,*-owning-memory): hook test phải tự lấy bộ nhớ từ C runtime.
        return std::malloc(request);
    }
    // NOLINTNEXTLINE(*-no-malloc,*-owning-memory): như trên.
    void* raw = std::malloc(request + alignment + kHeader);
    if (raw == nullptr) {
        return nullptr;
    }
    void* candidate = static_cast<std::byte*>(raw) + kHeader;
    usize space = request + alignment;
    // Luôn thành công vì khối dư đúng `alignment` byte sau phần đầu.
    void* result = std::align(alignment, request, candidate, space);
    std::memcpy(static_cast<std::byte*>(result) - kHeader, static_cast<const void*>(&raw),
                sizeof(raw));
    return result;
}

void release(void* pointer, const usize alignment) noexcept {
    if (pointer == nullptr) {
        return;
    }
    void* raw = pointer;
    if (alignment > alignof(std::max_align_t)) {
        std::memcpy(static_cast<void*>(&raw), static_cast<std::byte*>(pointer) - kHeader,
                    sizeof(raw));
    }
    // NOLINTNEXTLINE(*-no-malloc,*-owning-memory): trả khối do allocate() lấy từ malloc.
    std::free(raw);
}

void* allocate_or_abort(const usize size, const usize alignment) noexcept {
    void* result = allocate(size, alignment);
    // Exception bị tắt (X.3) nên không ném được std::bad_alloc; hết bộ nhớ trong test là dừng.
    if (result == nullptr) {
        std::abort();
    }
    return result;
}

#endif  // ORION_ALLOC_HOOKS

}  // namespace

u64 thread_allocation_count() noexcept {
    return g_thread_allocations;
}

}  // namespace orion::core::testing

#if ORION_ALLOC_HOOKS

// Tham số của các operator dưới đây cố ý không có const: clang 20.1.2 coi `operator delete(void*,
// const std::size_t)` là hàm giải phóng không thông thường và từ chối nó trong std::allocator
// (đo khi dựng module này), dù const ở tham số không đổi kiểu hàm.
// NOLINTBEGIN(misc-new-delete-overloads,cert-dcl54-cpp): thay đủ bộ operator toàn cục một lượt.
using orion::core::testing::allocate;
using orion::core::testing::allocate_or_abort;
using orion::core::testing::release;

void* operator new(std::size_t size) {
    return allocate_or_abort(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size) {
    return allocate_or_abort(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_or_abort(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_or_abort(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t& /*unused*/) noexcept {
    return allocate(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size, const std::nothrow_t& /*unused*/) noexcept {
    return allocate(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& /*unused*/) noexcept {
    return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& /*unused*/) noexcept {
    return allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer) noexcept {
    release(pointer, alignof(std::max_align_t));
}
void operator delete[](void* pointer) noexcept {
    release(pointer, alignof(std::max_align_t));
}
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    release(pointer, alignof(std::max_align_t));
}
void operator delete[](void* pointer, std::size_t /*size*/) noexcept {
    release(pointer, alignof(std::max_align_t));
}
void operator delete(void* pointer, std::align_val_t alignment) noexcept {
    release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t alignment) noexcept {
    release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete[](void* pointer, std::size_t /*size*/, std::align_val_t alignment) noexcept {
    release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, const std::nothrow_t& /*unused*/) noexcept {
    release(pointer, alignof(std::max_align_t));
}
void operator delete[](void* pointer, const std::nothrow_t& /*unused*/) noexcept {
    release(pointer, alignof(std::max_align_t));
}
void operator delete(void* pointer, std::align_val_t alignment,
                     const std::nothrow_t& /*unused*/) noexcept {
    release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment,
                       const std::nothrow_t& /*unused*/) noexcept {
    release(pointer, static_cast<std::size_t>(alignment));
}
// NOLINTEND(misc-new-delete-overloads,cert-dcl54-cpp)

#endif  // ORION_ALLOC_HOOKS
