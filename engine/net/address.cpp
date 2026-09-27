#include "engine/net/address.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace orion::net {
namespace {

// Dài nhất hợp lệ: "[" + 39 ký tự IPv6 (45 với IPv4 nhúng) + "]:65535".
constexpr usize kMaxAddressText = 64;

[[nodiscard]] std::unexpected<Error> invalid() noexcept {
    return fail(ErrorCode::InvalidArgument, "address: sai dạng");
}

// Số thập phân không dấu, không số 0 thừa ở đầu, tối đa `max`.
[[nodiscard]] std::optional<u32> parse_decimal(const std::string_view text,
                                               const u32 max) noexcept {
    if (text.empty() || text.size() > 5 || (text.size() > 1 && text[0] == '0')) {
        return std::nullopt;
    }
    u32 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<u32>(c - '0');
    }
    if (value > max) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::array<u8, 4>> parse_ipv4(const std::string_view text) noexcept {
    std::array<u8, 4> octets{};
    usize start = 0;
    for (usize i = 0; i < octets.size(); ++i) {
        const usize dot = text.find('.', start);
        const bool last = i + 1 == octets.size();
        if (last != (dot == std::string_view::npos)) {
            return std::nullopt;
        }
        const std::optional<u32> value =
            parse_decimal(text.substr(start, last ? std::string_view::npos : dot - start), 255);
        if (!value) {
            return std::nullopt;
        }
        octets[i] = static_cast<u8>(*value);
        start = dot + 1;
    }
    return octets;
}

[[nodiscard]] std::optional<u16> parse_hex_group(const std::string_view text) noexcept {
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    u32 value = 0;
    for (const char c : text) {
        u32 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u32>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u32>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<u32>(c - 'A') + 10;
        } else {
            return std::nullopt;
        }
        value = (value * 16) + digit;
    }
    return static_cast<u16>(value);
}

struct Groups {
    std::array<u16, 8> values{};
    usize count = 0;
};

// Dãy nhóm cách nhau bởi một ':'; phần tử cuối có thể là IPv4 nhúng (tính là hai nhóm).
[[nodiscard]] std::optional<Groups> parse_groups(const std::string_view text) noexcept {
    Groups groups;
    if (text.empty()) {
        return groups;
    }
    usize start = 0;
    while (true) {
        const usize colon = text.find(':', start);
        const bool last = colon == std::string_view::npos;
        const std::string_view part =
            text.substr(start, last ? std::string_view::npos : colon - start);
        if (last && part.find('.') != std::string_view::npos) {
            const std::optional<std::array<u8, 4>> ipv4 = parse_ipv4(part);
            if (!ipv4 || groups.count + 2 > groups.values.size()) {
                return std::nullopt;
            }
            groups.values[groups.count++] = static_cast<u16>(((*ipv4)[0] << 8U) | (*ipv4)[1]);
            groups.values[groups.count++] = static_cast<u16>(((*ipv4)[2] << 8U) | (*ipv4)[3]);
            return groups;
        }
        const std::optional<u16> group = parse_hex_group(part);
        if (!group || groups.count == groups.values.size()) {
            return std::nullopt;
        }
        groups.values[groups.count++] = *group;
        if (last) {
            return groups;
        }
        start = colon + 1;
    }
}

[[nodiscard]] std::optional<std::array<u8, 16>> parse_ipv6(const std::string_view text) noexcept {
    std::array<u16, 8> values{};
    const usize gap = text.find("::");
    if (gap == std::string_view::npos) {
        const std::optional<Groups> groups = parse_groups(text);
        if (!groups || groups->count != values.size()) {
            return std::nullopt;
        }
        values = groups->values;
    } else {
        if (text.find("::", gap + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view head_text = text.substr(0, gap);
        // IPv4 nhúng chỉ được ở cuối cả địa chỉ, nên không được nằm trước "::".
        if (head_text.find('.') != std::string_view::npos) {
            return std::nullopt;
        }
        const std::optional<Groups> head = parse_groups(head_text);
        const std::optional<Groups> tail = parse_groups(text.substr(gap + 2));
        // "::" thay cho ít nhất một nhóm 0.
        if (!head || !tail || head->count + tail->count > values.size() - 1) {
            return std::nullopt;
        }
        for (usize i = 0; i < head->count; ++i) {
            values[i] = head->values[i];
        }
        for (usize i = 0; i < tail->count; ++i) {
            values[values.size() - tail->count + i] = tail->values[i];
        }
    }
    std::array<u8, 16> bytes{};
    for (usize i = 0; i < values.size(); ++i) {
        bytes[2 * i] = static_cast<u8>(values[i] >> 8U);
        bytes[(2 * i) + 1] = static_cast<u8>(values[i] & 0xFFU);
    }
    return bytes;
}

void append_hex_group(std::string& out, const u16 value) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    bool started = false;
    for (u32 shift = 12;; shift -= 4) {
        const u32 digit = (value >> shift) & 0xFU;
        if (digit != 0 || started || shift == 0) {
            out.push_back(kDigits[digit]);
            started = true;
        }
        if (shift == 0) {
            break;
        }
    }
}

struct ZeroRun {
    usize start = 8;
    usize length = 0;
};

// Dãy nhóm 0 dài nhất, ít nhất hai nhóm; dãy đầu tiên khi bằng nhau (RFC 5952 mục 4.2). Không có
// thì start = 8.
[[nodiscard]] ZeroRun longest_zero_run(const std::array<u16, 8>& groups) noexcept {
    ZeroRun best;
    for (usize i = 0; i < groups.size();) {
        usize length = 0;
        while (i + length < groups.size() && groups[i + length] == 0) {
            ++length;
        }
        if (length >= 2 && length > best.length) {
            best = ZeroRun{i, length};
        }
        i += length == 0 ? 1 : length;
    }
    return best;
}

void append_ipv6(std::string& out, const std::array<u8, 16>& bytes) {
    std::array<u16, 8> groups{};
    for (usize i = 0; i < groups.size(); ++i) {
        groups[i] = static_cast<u16>((bytes[2 * i] << 8U) | bytes[(2 * i) + 1]);
    }
    const ZeroRun gap = longest_zero_run(groups);
    for (usize i = 0; i < groups.size();) {
        if (i == gap.start) {
            out += "::";
            i += gap.length;
            continue;
        }
        if (i > 0 && i != gap.start + gap.length) {
            out.push_back(':');
        }
        append_hex_group(out, groups[i]);
        ++i;
    }
}

}  // namespace

Result<Address> Address::parse(const std::string_view text) noexcept {
    if (text.size() > kMaxAddressText) {
        return invalid();
    }
    if (text.starts_with('[')) {
        const usize close = text.find("]:");
        if (close == std::string_view::npos) {
            return invalid();
        }
        const std::optional<std::array<u8, 16>> bytes = parse_ipv6(text.substr(1, close - 1));
        const std::optional<u32> port = parse_decimal(text.substr(close + 2), 65'535);
        if (!bytes || !port) {
            return invalid();
        }
        return v6(*bytes, static_cast<u16>(*port));
    }
    const usize colon = text.find(':');
    if (colon == std::string_view::npos) {
        return invalid();
    }
    const std::optional<std::array<u8, 4>> octets = parse_ipv4(text.substr(0, colon));
    const std::optional<u32> port = parse_decimal(text.substr(colon + 1), 65'535);
    if (!octets || !port) {
        return invalid();
    }
    return v4(*octets, static_cast<u16>(*port));
}

std::string Address::to_string() const {
    std::string out;
    if (family_ == AddressFamily::V4) {
        for (usize i = 0; i < 4; ++i) {
            if (i > 0) {
                out.push_back('.');
            }
            out += std::to_string(bytes_[i]);
        }
    } else {
        out.push_back('[');
        append_ipv6(out, bytes_);
        out.push_back(']');
    }
    out.push_back(':');
    out += std::to_string(port_);
    return out;
}

}  // namespace orion::net
