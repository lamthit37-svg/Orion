#include "engine/crypto/short_hash.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/types.hpp"

#include <sodium/crypto_shorthash.h>

#include <array>
#include <cstddef>
#include <span>

namespace orion::crypto {

static_assert(crypto_shorthash_KEYBYTES == kShortHashKeySize);
static_assert(crypto_shorthash_BYTES == sizeof(u64));

ShortHashKey generate_short_hash_key() noexcept {
    ShortHashKey key;
    crypto_shorthash_keygen(core::as_writable_uchars(key.mutable_view()).data());
    return key;
}

u64 short_hash(const std::span<const std::byte> data, const ShortHashKey& key) noexcept {
    std::array<std::byte, sizeof(u64)> out{};
    const int status = crypto_shorthash(core::as_writable_uchars(std::span(out)).data(),
                                        core::as_uchars(data).data(), data.size(),
                                        core::as_uchars(key.view()).data());
    ORION_VERIFY(status == 0, "crypto_shorthash trả {}", status);
    return core::load_le<u64>(std::span<const std::byte, sizeof(u64)>(out));
}

}  // namespace orion::crypto
