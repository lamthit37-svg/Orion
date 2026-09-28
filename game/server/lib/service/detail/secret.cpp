#include "game/server/lib/service/detail/secret.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "game/server/lib/service/service.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace orion::service::detail {
namespace {

// Bản ship từ chối bí mật dev ở mọi môi trường (CLAUDE.md X.9). ORION_SHIP do
// cmake/orion_flags.cmake đặt 0 hay 1 cho mọi target (-Wundef bắt chỗ quên).
[[nodiscard]] constexpr bool ship_build() noexcept {
#if ORION_SHIP
    return true;
#else
    return false;
#endif
}

}  // namespace

bool is_dev_secret(const std::string_view path) noexcept {
    const usize slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return name.find(".dev.") != std::string_view::npos;
}

Result<void> allow_secret(const std::string_view path, const Environment environment) noexcept {
    if (!is_dev_secret(path)) {
        return {};
    }
    if (ship_build() || environment != Environment::Local) {
        return fail(ErrorCode::FailedPrecondition,
                    "service: bí mật dev (*.dev.*) chỉ dùng với environment local");
    }
    return {};
}

Result<std::string> read_secret_file(const std::string_view path, const Environment environment) {
    if (Result<void> allowed = allow_secret(path, environment); !allowed) {
        return std::unexpected(allowed.error());
    }
    Result<std::string> text = read_file(path, kMaxSecretFileBytes);
    if (!text) {
        return std::unexpected(text.error());
    }
    std::string& secret = *text;
    if (secret.ends_with('\n')) {
        secret.pop_back();
        if (secret.ends_with('\r')) {
            secret.pop_back();
        }
    }
    if (secret.empty()) {
        return fail(ErrorCode::InvalidArgument, "service: tệp bí mật rỗng");
    }
    // Trả nguyên Result: bộ đệm của chuỗi đi theo, không để lại bản chép.
    return text;
}

void wipe(std::string& text) noexcept {
    // Cả phần dung lượng chưa dùng: pop_back ở read_secret_file để lại byte cũ ở đó.
    text.resize(text.capacity());
    crypto::secure_zero(std::as_writable_bytes(std::span(text)));
    text.clear();
}

}  // namespace orion::service::detail
