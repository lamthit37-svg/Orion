#pragma once

// Địa chỉ IP và cổng của một điểm cuối UDP, IPv4 hoặc IPv6. Kết nối được nhận diện bằng connection
// id, không bằng địa chỉ (CLAUDE.md X.9); địa chỉ chỉ là nơi gửi gói tới và nơi gói vừa tới.
//
// parse đọc dạng chữ trong cấu hình và dòng lệnh: dữ liệu từ ngoài, nên là hàm toàn phần và có
// fuzz target net_address. to_string cho dạng chữ chuẩn (RFC 5952 cho IPv6), nên
// parse(to_string(a)) == a.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace orion::net {

enum class AddressFamily : u8 {
    V4 = 4,
    V6 = 6,
};

class Address {
public:
    // 0.0.0.0:0.
    constexpr Address() noexcept = default;

    [[nodiscard]] static constexpr Address v4(const std::array<u8, 4>& octets,
                                              const u16 port) noexcept {
        Address address;
        for (usize i = 0; i < octets.size(); ++i) {
            address.bytes_[i] = octets[i];
        }
        address.port_ = port;
        return address;
    }
    [[nodiscard]] static constexpr Address v6(const std::array<u8, 16>& bytes,
                                              const u16 port) noexcept {
        Address address;
        address.bytes_ = bytes;
        address.family_ = AddressFamily::V6;
        address.port_ = port;
        return address;
    }
    [[nodiscard]] static constexpr Address loopback_v4(const u16 port) noexcept {
        return v4({127, 0, 0, 1}, port);
    }
    [[nodiscard]] static constexpr Address loopback_v6(const u16 port) noexcept {
        std::array<u8, 16> bytes{};
        bytes[15] = 1;
        return v6(bytes, port);
    }

    // "a.b.c.d:port" hoặc "[ipv6]:port"; cổng thập phân 0..65535, không có số 0 thừa ở đầu. IPv6
    // theo RFC 4291 mục 2.2: nhóm hex 1 tới 4 chữ số, "::" tối đa một lần, IPv4 nhúng ở cuối; không
    // nhận zone ("%eth0") và tên miền. Lỗi: InvalidArgument.
    [[nodiscard]] static Result<Address> parse(std::string_view text) noexcept;

    [[nodiscard]] AddressFamily family() const noexcept { return family_; }
    [[nodiscard]] u16 port() const noexcept { return port_; }
    // 4 byte với IPv4, 16 byte với IPv6, theo thứ tự mạng.
    [[nodiscard]] std::span<const u8> bytes() const noexcept {
        return std::span(bytes_).first(family_ == AddressFamily::V4 ? 4 : 16);
    }

    // Dạng chữ chuẩn: "a.b.c.d:port", hay "[ipv6]:port" với IPv6 viết thường, bỏ số 0 đầu nhóm,
    // nén dãy nhóm 0 dài nhất (ít nhất hai nhóm, dãy đầu tiên khi bằng nhau) thành "::" (RFC 5952
    // mục 4); luôn viết 8 nhóm hex, không dùng dạng IPv4 nhúng.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend constexpr bool operator==(const Address& a,
                                                   const Address& b) noexcept = default;

private:
    std::array<u8, 16> bytes_{};
    u16 port_ = 0;
    AddressFamily family_ = AddressFamily::V4;
};

}  // namespace orion::net
