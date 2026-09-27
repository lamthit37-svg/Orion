#pragma once

// Băm mật khẩu Argon2id (X.9). Chuỗi kết quả theo định dạng PHC của libsodium
// ("$argon2id$v=19$m=...,t=...,p=1$<salt>$<hash>"): mang theo salt và tham số, nên lưu thẳng vào DB
// và kiểm lại được sau khi đổi tham số.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <string_view>

namespace orion::crypto {

// Mật khẩu là dữ liệu từ ngoài; dài hơn giới hạn này thì bị từ chối trước khi băm.
inline constexpr usize kMaxPasswordBytes = 256;
// Cỡ tối đa của chuỗi băm, gồm cả ký tự kết thúc (crypto_pwhash_STRBYTES).
inline constexpr usize kPasswordHashCapacity = 128;

// Trần tham số, cho cả chuỗi băm đọc từ ngoài lẫn hash_password: 1 GiB (mức "sensitive" của
// libsodium) và 10 lượt.
inline constexpr u64 kMaxPasswordMemoryKib = 1'048'576;
inline constexpr u64 kMaxPasswordOperations = 10;

struct PasswordLimits {
    u64 operations = 0;
    usize memory_bytes = 0;
};

// Mức "interactive" của libsodium (2 lượt, 64 MiB): mặc định cho dịch vụ auth.
[[nodiscard]] PasswordLimits interactive_password_limits() noexcept;
// Mức nhỏ nhất libsodium cho phép; chỉ để test chạy nhanh, không bao giờ cho mật khẩu thật.
[[nodiscard]] PasswordLimits minimum_password_limits() noexcept;

class PasswordHash {
public:
    // Kiểm chuỗi đọc từ DB (dữ liệu từ ngoài, X.5) trước khi libsodium dùng nó: đúng dạng mà
    // hash_password sinh ra, "$argon2id$v=19$m=<KiB>,t=<lượt>,p=1$<salt>$<hash>" với salt 22 và
    // hash 43 ký tự base64, và tham số không vượt kMaxPasswordMemoryKib, kMaxPasswordOperations,
    // vì libsodium cấp bộ nhớ và chạy số lượt theo đúng tham số trong chuỗi. Sai thì
    // InvalidArgument.
    [[nodiscard]] static Result<PasswordHash> parse(std::string_view encoded) noexcept;

    [[nodiscard]] std::string_view view() const noexcept { return {text_.data(), length_}; }
    // Cùng nội dung với view(), luôn kết thúc bằng '\0' cho API C.
    [[nodiscard]] const char* c_str() const noexcept { return text_.data(); }

private:
    friend Result<PasswordHash> hash_password(std::string_view password,
                                              const PasswordLimits& limits) noexcept;

    // length_ < kPasswordHashCapacity, nên text_ luôn còn ít nhất một '\0' ở cuối.
    std::array<char, kPasswordHashCapacity> text_{};
    usize length_ = 0;
};

// Salt ngẫu nhiên mỗi lần, nên cùng mật khẩu cho chuỗi khác nhau. Lỗi: InvalidArgument khi mật khẩu
// quá dài hoặc tham số ngoài khoảng parse nhận (1..10 lượt, 8 KiB..1 GiB); ResourceExhausted khi
// không cấp được bộ nhớ.
[[nodiscard]] Result<PasswordHash> hash_password(std::string_view password,
                                                 const PasswordLimits& limits) noexcept;

// Thời gian không phụ thuộc mật khẩu đúng tới đâu. Mật khẩu quá dài cho false.
[[nodiscard]] bool verify_password(const PasswordHash& hash, std::string_view password) noexcept;

// true khi hash được tạo với tham số khác `limits`: đăng nhập thành công thì băm lại bằng tham số
// mới.
[[nodiscard]] bool password_needs_rehash(const PasswordHash& hash,
                                         const PasswordLimits& limits) noexcept;

}  // namespace orion::crypto
