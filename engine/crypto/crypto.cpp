#include "engine/crypto/crypto.hpp"

#include "engine/core/error.hpp"

#include <sodium/core.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>

#include <cstddef>
#include <span>

namespace orion::crypto {

Result<void> initialize() noexcept {
    // 0: vừa khởi tạo; 1: đã khởi tạo trước đó; -1: lỗi (ví dụ không đọc được nguồn ngẫu nhiên).
    if (sodium_init() < 0) {
        return fail(ErrorCode::Internal, "crypto: sodium_init thất bại");
    }
    return {};
}

void random_bytes(const std::span<std::byte> out) noexcept {
    randombytes_buf(out.data(), out.size());
}

bool equal_constant_time(const std::span<const std::byte> a,
                         const std::span<const std::byte> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

void secure_zero(const std::span<std::byte> data) noexcept {
    sodium_memzero(data.data(), data.size());
}

}  // namespace orion::crypto
