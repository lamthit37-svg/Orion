// Biến môi trường trên Windows: GetEnvironmentVariableW, đổi tên từ UTF-8 sang UTF-16 và giá trị
// từ UTF-16 sang UTF-8. std::getenv trả chuỗi theo code page ANSI và bị MSVC đánh dấu không an toàn
// (C4996).

#include "engine/core/environment.hpp"

#include "engine/core/types.hpp"

#include <windows.h>

#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace orion::core {
namespace {

[[nodiscard]] std::optional<std::wstring> to_utf16(const std::string_view text) {
    if (text.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int size = static_cast<int>(text.size());
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<usize>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, wide.data(),
                            length) != length) {
        return std::nullopt;
    }
    return wide;
}

[[nodiscard]] std::optional<std::string> to_utf8(const std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    if (text.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int size = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::nullopt;
    }
    std::string narrow(static_cast<usize>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, narrow.data(), length,
                            nullptr, nullptr) != length) {
        return std::nullopt;
    }
    return narrow;
}

}  // namespace

std::optional<std::string> environment_variable(const std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<std::wstring> key = to_utf16(name);
    if (!key) {
        return std::nullopt;
    }
    // Hỏi cỡ (gồm NUL cuối) rồi đọc. Trả 0 khi biến không có. Biến có thể dài ra giữa hai lần gọi
    // nếu ai đó đổi nó; khi đó đọc lại với cỡ mới, tối đa vài lần.
    DWORD needed = GetEnvironmentVariableW(key->c_str(), nullptr, 0);
    for (u32 attempt = 0; attempt < 4 && needed != 0; ++attempt) {
        std::wstring value(needed, L'\0');
        const DWORD written = GetEnvironmentVariableW(key->c_str(), value.data(), needed);
        if (written < needed) {
            value.resize(written);
            return to_utf8(value);
        }
        needed = written;
    }
    return std::nullopt;
}

}  // namespace orion::core
