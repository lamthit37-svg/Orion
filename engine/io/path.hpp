#pragma once

// Đường dẫn ảo của VFS và pak (docs/formats/pak.md, mục "Đường dẫn ảo"): tương đối, ngăn cách bằng
// '/', chỉ gồm chữ thường ASCII, chữ số và "._-". Luật chặt để một chuỗi trỏ tới đúng một tệp trên
// mọi hệ điều hành, kể cả hệ thống tệp không phân biệt hoa thường (Windows, macOS), và để tệp rời ở
// DEV và entry trong pak ở CHƠI dùng chung một tên.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <compare>
#include <string_view>

namespace orion::io {

// Đủ cho tên do cooker sinh ra, và giữ đường dẫn gốc (thư mục cooked cộng đường dẫn ảo) dưới giới
// hạn 260 ký tự của Windows khi thư mục gốc không quá sâu.
inline constexpr usize kMaxVirtualPathBytes = 200;

// true khi `text` đúng luật: 1 tới kMaxVirtualPathBytes byte trong [a-z0-9._-/]; không mở đầu hay
// kết thúc bằng '/', không có "//"; không đoạn nào bắt đầu hay kết thúc bằng '.'; không đoạn nào có
// phần trước dấu chấm đầu tiên là tên thiết bị của Windows (con, prn, aux, nul, com0-9, lpt0-9).
[[nodiscard]] bool is_valid_virtual_path(std::string_view text) noexcept;

// Đường dẫn ảo đã kiểm, chứa ngay trong đối tượng (không cấp phát). So sánh theo byte.
class VirtualPath {
public:
    // Dữ liệu từ ngoài đi thẳng vào được: sai luật trả InvalidArgument.
    [[nodiscard]] static Result<VirtualPath> parse(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const noexcept { return {text_.data(), length_}; }

    [[nodiscard]] friend bool operator==(const VirtualPath& a, const VirtualPath& b) noexcept {
        return a.view() == b.view();
    }
    [[nodiscard]] friend std::strong_ordering operator<=>(const VirtualPath& a,
                                                          const VirtualPath& b) noexcept {
        return a.view() <=> b.view();
    }

private:
    VirtualPath() = default;

    std::array<char, kMaxVirtualPathBytes> text_{};
    usize length_ = 0;
};

}  // namespace orion::io
