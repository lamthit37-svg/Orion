#include "engine/crypto/password.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <sodium/crypto_pwhash.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <string_view>
#include <system_error>

namespace orion::crypto {
namespace {

static_assert(crypto_pwhash_STRBYTES == kPasswordHashCapacity);
// Trần của parse nằm trong khoảng libsodium nhận, và bằng mức "sensitive" của nó.
static_assert(crypto_pwhash_OPSLIMIT_MAX >= kMaxPasswordOperations);
static_assert(crypto_pwhash_MEMLIMIT_MAX / 1024 >= kMaxPasswordMemoryKib);
static_assert(crypto_pwhash_MEMLIMIT_SENSITIVE == kMaxPasswordMemoryKib * 1024);

constexpr u64 kMinMemoryKib = crypto_pwhash_MEMLIMIT_MIN / 1024;
constexpr u64 kMinOperations = crypto_pwhash_OPSLIMIT_MIN;

// Salt 16 byte và hash 32 byte của crypto_pwhash_str, viết bằng base64 không đệm.
static_assert(crypto_pwhash_SALTBYTES == 16);
constexpr usize kSaltBase64Length = 22;
constexpr usize kHashBase64Length = 43;

constexpr std::string_view kPrefix = "$argon2id$v=19$m=";

// Đọc tuần tự một chuỗi PHC; mọi hàm trả false khi không khớp và không đọc quá cuối chuỗi.
class PhcCursor {
public:
    explicit PhcCursor(const std::string_view text) noexcept : rest_(text) {}

    [[nodiscard]] bool literal(const std::string_view expected) noexcept {
        if (!rest_.starts_with(expected)) {
            return false;
        }
        rest_.remove_prefix(expected.size());
        return true;
    }

    // Số thập phân không dấu, không số 0 đứng đầu, trong [low, high].
    [[nodiscard]] bool number(const u64 low, const u64 high) noexcept {
        u64 value = 0;
        const auto [end, error] = std::from_chars(rest_.data(), rest_.data() + rest_.size(), value);
        const auto length = static_cast<usize>(end - rest_.data());
        if (error != std::errc{} || length == 0 || (length > 1 && rest_.front() == '0')) {
            return false;
        }
        rest_.remove_prefix(length);
        return value >= low && value <= high;
    }

    // Đúng `length` ký tự base64 (bảng chữ chuẩn, không đệm).
    [[nodiscard]] bool base64(const usize length) noexcept {
        if (rest_.size() < length) {
            return false;
        }
        const bool valid = std::ranges::all_of(rest_.substr(0, length), [](const char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '+' || c == '/';
        });
        rest_.remove_prefix(length);
        return valid;
    }

    [[nodiscard]] bool at_end() const noexcept { return rest_.empty(); }

private:
    std::string_view rest_;
};

[[nodiscard]] bool well_formed(const std::string_view encoded) noexcept {
    PhcCursor cursor(encoded);
    return cursor.literal(kPrefix) && cursor.number(kMinMemoryKib, kMaxPasswordMemoryKib) &&
           cursor.literal(",t=") && cursor.number(kMinOperations, kMaxPasswordOperations) &&
           cursor.literal(",p=1$") && cursor.base64(kSaltBase64Length) && cursor.literal("$") &&
           cursor.base64(kHashBase64Length) && cursor.at_end();
}

// hash_password chỉ nhận tham số mà parse cũng nhận, để chuỗi nào tự băm ra cũng đọc lại được.
[[nodiscard]] bool supported(const PasswordLimits& limits) noexcept {
    return limits.operations >= kMinOperations && limits.operations <= kMaxPasswordOperations &&
           limits.memory_bytes / 1024 >= kMinMemoryKib &&
           limits.memory_bytes / 1024 <= kMaxPasswordMemoryKib;
}

}  // namespace

PasswordLimits interactive_password_limits() noexcept {
    return {crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE};
}

PasswordLimits minimum_password_limits() noexcept {
    return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN};
}

Result<PasswordHash> PasswordHash::parse(const std::string_view encoded) noexcept {
    if (encoded.size() >= kPasswordHashCapacity || !well_formed(encoded)) {
        return fail(ErrorCode::InvalidArgument, "password: chuỗi băm không phải Argon2id hợp lệ");
    }
    PasswordHash hash;
    std::ranges::copy(encoded, hash.text_.begin());
    hash.length_ = encoded.size();
    return hash;
}

Result<PasswordHash> hash_password(const std::string_view password,
                                   const PasswordLimits& limits) noexcept {
    if (password.size() > kMaxPasswordBytes) {
        return fail(ErrorCode::InvalidArgument, "password: mật khẩu quá dài",
                    static_cast<i64>(password.size()));
    }
    if (!supported(limits)) {
        return fail(ErrorCode::InvalidArgument, "password: tham số Argon2id ngoài khoảng");
    }
    PasswordHash hash;
    if (crypto_pwhash_str_alg(hash.text_.data(), password.data(), password.size(),
                              limits.operations, limits.memory_bytes,
                              crypto_pwhash_ALG_ARGON2ID13) != 0) {
        // Tham số đã kiểm ở trên, nên lỗi còn lại là không cấp được vùng nhớ của Argon2.
        return fail(ErrorCode::ResourceExhausted, "password: không cấp được bộ nhớ cho Argon2id",
                    static_cast<i64>(limits.memory_bytes));
    }
    hash.length_ = std::strlen(hash.text_.data());
    return hash;
}

bool verify_password(const PasswordHash& hash, const std::string_view password) noexcept {
    if (password.size() > kMaxPasswordBytes) {
        return false;
    }
    return crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
}

bool password_needs_rehash(const PasswordHash& hash, const PasswordLimits& limits) noexcept {
    // 0: đúng tham số; 1: tham số khác; -1: chuỗi hỏng, cũng cần băm lại.
    return crypto_pwhash_str_needs_rehash(hash.c_str(), limits.operations, limits.memory_bytes) !=
           0;
}

}  // namespace orion::crypto
