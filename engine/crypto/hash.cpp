#include "engine/crypto/hash.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/types.hpp"

#include <sodium/crypto_generichash.h>

#include <array>
#include <cstddef>
#include <span>

namespace orion::crypto {
namespace {

static_assert(crypto_generichash_BYTES == kHashSize);

// Vùng byte của Hasher chứa đối tượng crypto_generichash_state của libsodium: kiểu C chỉ gồm mảng
// byte nên được tạo ngầm trong vùng lưu trữ std::byte (C++20, P0593), truy cập qua void* như vùng
// nhớ từ malloc.
template <usize N>
[[nodiscard]] crypto_generichash_state* sodium_state(std::array<std::byte, N>& storage) noexcept {
    static_assert(sizeof(crypto_generichash_state) == N);
    static_assert(alignof(crypto_generichash_state) <= 64);
    void* raw = storage.data();
    return static_cast<crypto_generichash_state*>(raw);
}

}  // namespace

Hash hash(const std::span<const std::byte> data) noexcept {
    Hash result;
    const int status =
        crypto_generichash(core::as_writable_uchars(result.bytes).data(), result.bytes.size(),
                           core::as_uchars(data).data(), data.size(), nullptr, 0);
    ORION_VERIFY(status == 0, "crypto_generichash trả {}", status);
    return result;
}

Hasher::Hasher() noexcept {
    const int status = crypto_generichash_init(sodium_state(state_), nullptr, 0, kHashSize);
    ORION_VERIFY(status == 0, "crypto_generichash_init trả {}", status);
}

void Hasher::update(const std::span<const std::byte> data) noexcept {
    const int status =
        crypto_generichash_update(sodium_state(state_), core::as_uchars(data).data(), data.size());
    ORION_VERIFY(status == 0, "crypto_generichash_update trả {}", status);
}

Hash Hasher::finish() noexcept {
    Hash result;
    const int status = crypto_generichash_final(
        sodium_state(state_), core::as_writable_uchars(result.bytes).data(), result.bytes.size());
    // libsodium trả -1 khi finish lần hai: đó là lỗi lập trình của bên gọi.
    ORION_VERIFY(status == 0, "Hasher::finish gọi hai lần");
    return result;
}

}  // namespace orion::crypto
