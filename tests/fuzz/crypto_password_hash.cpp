// Fuzz PasswordHash::parse (CLAUDE.md X.4, X.9): chuỗi băm đọc từ DB là dữ liệu từ ngoài. Mọi dãy
// byte cho ra một PasswordHash hoặc InvalidArgument, không bao giờ UB. Chuỗi được nhận phải giữ
// nguyên nội dung, có '\0' ở cuối cho API C, và bộ giải mã của libsodium đọc được mà không cấp bộ
// nhớ theo tham số trong chuỗi (password_needs_rehash chỉ giải mã, không băm).

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/password.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orion::crypto::PasswordHash;
    static const bool kSodiumReady = orion::crypto::initialize().has_value();
    ORION_VERIFY(kSodiumReady, "sodium_init thất bại");

    // Chép sang chuỗi char thay vì ép kiểu con trỏ (X.3).
    std::string text(size, '\0');
    if (size > 0) {
        std::memcpy(text.data(), data, size);
    }
    const orion::Result<PasswordHash> hash = PasswordHash::parse(text);
    if (!hash.has_value()) {
        ORION_VERIFY(hash.error().code() == orion::ErrorCode::InvalidArgument,
                     "parse chỉ được trả InvalidArgument");
        return 0;
    }
    ORION_VERIFY(hash->view() == text, "parse phải giữ nguyên chuỗi");
    ORION_VERIFY(std::strlen(hash->c_str()) == text.size(), "c_str phải kết thúc đúng chỗ");
    static_cast<void>(
        orion::crypto::password_needs_rehash(*hash, orion::crypto::minimum_password_limits()));
    return 0;
}
