#include "engine/core/utf8.hpp"

#include "engine/core/types.hpp"

#include <string_view>

namespace orion::core {
namespace {

[[nodiscard]] constexpr u8 byte_at(const std::string_view text, const usize index) noexcept {
    return static_cast<u8>(text[index]);
}

[[nodiscard]] constexpr bool is_continuation(const u8 byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

// Độ dài chuỗi byte mà byte đầu `lead` mở ra, và khoảng hợp lệ của byte thứ hai (RFC 3629 bảng 3-7,
// dạng thu gọn): khoảng hẹp ở byte thứ hai là thứ loại mã hoá thừa, surrogate và điểm mã quá lớn.
struct Lead {
    usize length;
    u8 second_min;
    u8 second_max;
};

[[nodiscard]] constexpr Lead classify(const u8 lead) noexcept {
    if (lead < 0x80U) {
        return {1, 0, 0};
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return {2, 0x80U, 0xBFU};
    }
    if (lead == 0xE0U) {
        return {3, 0xA0U, 0xBFU};
    }
    if ((lead >= 0xE1U && lead <= 0xECU) || lead == 0xEEU || lead == 0xEFU) {
        return {3, 0x80U, 0xBFU};
    }
    if (lead == 0xEDU) {
        return {3, 0x80U, 0x9FU};  // U+D800..U+DFFF là surrogate.
    }
    if (lead == 0xF0U) {
        return {4, 0x90U, 0xBFU};
    }
    if (lead >= 0xF1U && lead <= 0xF3U) {
        return {4, 0x80U, 0xBFU};
    }
    if (lead == 0xF4U) {
        return {4, 0x80U, 0x8FU};  // Không vượt U+10FFFF.
    }
    return {0, 0, 0};  // 0x80..0xC1 và 0xF5..0xFF không bao giờ mở đầu một điểm mã.
}

}  // namespace

usize utf8_sequence_length(const std::string_view text, const usize offset) noexcept {
    const Lead lead = classify(byte_at(text, offset));
    if (lead.length == 0 || text.size() - offset < lead.length) {
        return 0;
    }
    if (lead.length > 1) {
        const u8 second = byte_at(text, offset + 1);
        if (second < lead.second_min || second > lead.second_max) {
            return 0;
        }
        for (usize k = 2; k < lead.length; ++k) {
            if (!is_continuation(byte_at(text, offset + k))) {
                return 0;
            }
        }
    }
    return lead.length;
}

bool is_valid_utf8(const std::string_view text) noexcept {
    usize i = 0;
    while (i < text.size()) {
        const usize length = utf8_sequence_length(text, i);
        if (length == 0) {
            return false;
        }
        i += length;
    }
    return true;
}

usize utf8_length(const std::string_view text) noexcept {
    usize count = 0;
    for (usize i = 0; i < text.size(); ++i) {
        if (!is_continuation(byte_at(text, i))) {
            ++count;
        }
    }
    return count;
}

std::string_view utf8_truncate(const std::string_view text, const usize max_bytes) noexcept {
    if (text.size() <= max_bytes) {
        return text;
    }
    usize end = max_bytes;
    // Lùi khỏi các byte nối để `end` rơi vào đầu một điểm mã; UTF-8 hợp lệ có tối đa 3 byte nối.
    while (end > 0 && is_continuation(byte_at(text, end))) {
        --end;
    }
    return text.substr(0, end);
}

std::string_view utf8_drop_incomplete_tail(const std::string_view text) noexcept {
    // Tìm byte mở đầu của điểm mã cuối cùng: lùi qua tối đa 3 byte nối.
    usize lead = text.size();
    for (usize back = 1; back <= 4 && back <= text.size(); ++back) {
        if (!is_continuation(byte_at(text, text.size() - back))) {
            lead = text.size() - back;
            break;
        }
    }
    if (lead == text.size()) {
        return text;  // Toàn byte nối hoặc rỗng: không có điểm mã dở dang nào để bỏ.
    }
    const usize needed = classify(byte_at(text, lead)).length;
    return needed > text.size() - lead ? text.substr(0, lead) : text;
}

}  // namespace orion::core
