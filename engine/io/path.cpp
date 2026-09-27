#include "engine/io/path.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace orion::io {
namespace {

[[nodiscard]] constexpr bool allowed_character(const char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// Windows coi các tên này là thiết bị, kể cả khi có đuôi ("nul.txt"), nên tệp mang tên đó không tạo
// hay mở được như tệp thường.
[[nodiscard]] bool windows_device_name(const std::string_view stem) noexcept {
    constexpr std::array<std::string_view, 4> kPlain = {"con", "prn", "aux", "nul"};
    if (std::ranges::find(kPlain, stem) != kPlain.end()) {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
           stem[3] >= '0' && stem[3] <= '9';
}

[[nodiscard]] bool valid_segment(const std::string_view segment) noexcept {
    if (segment.empty() || segment.front() == '.' || segment.back() == '.') {
        return false;
    }
    if (!std::ranges::all_of(segment, allowed_character)) {
        return false;
    }
    return !windows_device_name(segment.substr(0, segment.find('.')));
}

}  // namespace

bool is_valid_virtual_path(const std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxVirtualPathBytes) {
        return false;
    }
    std::string_view rest = text;
    while (true) {
        const usize slash = rest.find('/');
        if (!valid_segment(rest.substr(0, slash))) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        rest.remove_prefix(slash + 1);
    }
}

Result<VirtualPath> VirtualPath::parse(const std::string_view text) noexcept {
    if (!is_valid_virtual_path(text)) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn ảo sai luật",
                    static_cast<i64>(text.size()));
    }
    VirtualPath path;
    std::ranges::copy(text, path.text_.begin());
    path.length_ = text.size();
    return path;
}

}  // namespace orion::io
