#pragma once

// BLAKE2b 256 bit (ARCH §4.4: manifest và pak theo hash BLAKE2b). Không khoá; không dùng làm MAC
// hay để băm mật khẩu (mật khẩu đi qua password.hpp).

#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace orion::crypto {

inline constexpr usize kHashSize = 32;

struct HashTag;
using Hash = PublicBytes<kHashSize, HashTag>;

[[nodiscard]] Hash hash(std::span<const std::byte> data) noexcept;

// Băm từng phần cho dữ liệu lớn đọc theo khối (pak). Gọi update bao nhiêu lần cũng được, rồi finish
// đúng một lần. Không cấp phát: trạng thái của libsodium nằm ngay trong đối tượng.
class Hasher {
public:
    Hasher() noexcept;
    void update(std::span<const std::byte> data) noexcept;
    [[nodiscard]] Hash finish() noexcept;

private:
    // Đúng cỡ và căn lề của crypto_generichash_state (kiểm bằng static_assert trong hash.cpp). Cỡ
    // là bội của căn lề nên lớp không có byte đệm.
    alignas(64) std::array<std::byte, 384> state_{};
};

}  // namespace orion::crypto
