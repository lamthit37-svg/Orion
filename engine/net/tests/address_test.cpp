#include "engine/net/address.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace orion::net {
namespace {

// Bảng lấy từ module ipaddress của Python 3.11.15 (tham chiếu độc lập): đầu vào, dạng đầy đủ
// (IPv6Address.exploded) và dạng rút gọn chuẩn (IPv6Address.compressed, theo RFC 5952).
struct Ipv6Case {
    std::string_view input;
    std::string_view exploded;
    std::string_view compressed;
};

constexpr std::array kIpv6Cases = {
    Ipv6Case{"::", "0000:0000:0000:0000:0000:0000:0000:0000", "::"},
    Ipv6Case{"::1", "0000:0000:0000:0000:0000:0000:0000:0001", "::1"},
    Ipv6Case{"1::", "0001:0000:0000:0000:0000:0000:0000:0000", "1::"},
    Ipv6Case{"2001:db8::1", "2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"},
    Ipv6Case{"2001:0db8:0000:0000:0000:ff00:0042:8329", "2001:0db8:0000:0000:0000:ff00:0042:8329",
             "2001:db8::ff00:42:8329"},
    Ipv6Case{"2001:db8:0:0:1:0:0:1", "2001:0db8:0000:0000:0001:0000:0000:0001",
             "2001:db8::1:0:0:1"},
    Ipv6Case{"2001:DB8::A", "2001:0db8:0000:0000:0000:0000:0000:000a", "2001:db8::a"},
    Ipv6Case{"fe80::1:2:3:4", "fe80:0000:0000:0000:0001:0002:0003:0004", "fe80::1:2:3:4"},
    Ipv6Case{"1:0:0:2:0:0:3:4", "0001:0000:0000:0002:0000:0000:0003:0004", "1::2:0:0:3:4"},
    Ipv6Case{"0:0:0:0:0:0:0:0", "0000:0000:0000:0000:0000:0000:0000:0000", "::"},
    Ipv6Case{"1:2:3:4:5:6:7::", "0001:0002:0003:0004:0005:0006:0007:0000", "1:2:3:4:5:6:7:0"},
    Ipv6Case{"::2:3:4:5:6:7:8", "0000:0002:0003:0004:0005:0006:0007:0008", "0:2:3:4:5:6:7:8"},
    Ipv6Case{"::1.2.3.4", "0000:0000:0000:0000:0000:0000:0102:0304", "::102:304"},
    Ipv6Case{"1:2:3:4:5:6:1.2.3.4", "0001:0002:0003:0004:0005:0006:0102:0304",
             "1:2:3:4:5:6:102:304"},
    Ipv6Case{"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
             "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"},
    Ipv6Case{"0:0:1:0:0:0:1:0", "0000:0000:0001:0000:0000:0000:0001:0000", "0:0:1::1:0"},
    Ipv6Case{"1:0:2:0:3:0:4:0", "0001:0000:0002:0000:0003:0000:0004:0000", "1:0:2:0:3:0:4:0"},
};

// Cũng bị ipaddress của Python từ chối.
constexpr std::array<std::string_view, 17> kBadIpv6 = {
    ":",   ":::",       "1::2::3", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7",    "12345::",
    "g::", "1.2.3.4::", "::1.2.3", "::1.2.3.256",       "::01.2.3.4",       "1::2:3:4:5:6:7:8",
    "",    "1:",        ":1",      "1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8"};

[[nodiscard]] std::string exploded(const Address& address) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    const auto bytes = address.bytes();
    for (usize i = 0; i < bytes.size(); ++i) {
        if (i > 0 && i % 2 == 0) {
            text.push_back(':');
        }
        text.push_back(kDigits[bytes[i] >> 4U]);
        text.push_back(kDigits[bytes[i] & 0x0FU]);
    }
    return text;
}

TEST(Address, Ipv6MatchesPythonIpaddress) {
    for (const Ipv6Case& ipv6 : kIpv6Cases) {
        const std::string text = "[" + std::string(ipv6.input) + "]:443";
        const Result<Address> address = Address::parse(text);
        ASSERT_TRUE(address.has_value()) << text;
        EXPECT_EQ(address->family(), AddressFamily::V6);
        EXPECT_EQ(address->port(), 443U);
        EXPECT_EQ(exploded(*address), ipv6.exploded) << text;
        EXPECT_EQ(address->to_string(), "[" + std::string(ipv6.compressed) + "]:443") << text;
        // Dạng chữ chuẩn đọc lại ra đúng địa chỉ.
        EXPECT_EQ(Address::parse(address->to_string()).value_or(Address()), *address) << text;
    }
    for (const std::string_view bad : kBadIpv6) {
        const std::string text = "[" + std::string(bad) + "]:443";
        const Result<Address> address = Address::parse(text);
        ASSERT_FALSE(address.has_value()) << text;
        EXPECT_EQ(address.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(Address, Ipv4AndPorts) {
    for (const std::string_view good :
         {"0.0.0.0:0", "255.255.255.255:65535", "127.0.0.1:80", "192.168.1.100:7777"}) {
        const Result<Address> address = Address::parse(good);
        ASSERT_TRUE(address.has_value()) << good;
        EXPECT_EQ(address->family(), AddressFamily::V4);
        EXPECT_EQ(address->to_string(), good);
    }
    const Result<Address> local = Address::parse("127.0.0.1:9000");
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(*local, Address::loopback_v4(9000));
    EXPECT_EQ(Address::parse("[::1]:9000").value_or(Address()), Address::loopback_v6(9000));
    for (const std::string_view bad : {"256.0.0.1:1",
                                       "1.2.3:1",
                                       "1.2.3.4.5:1",
                                       "01.2.3.4:1",
                                       "1.2.3.-4:1",
                                       "1..3.4:1",
                                       "a.b.c.d:1",
                                       " 1.2.3.4:1",
                                       "1.2.3.4",
                                       "1.2.3.4:",
                                       "1.2.3.4:65536",
                                       "1.2.3.4:080",
                                       "1.2.3.4:-1",
                                       "1.2.3.4:8o",
                                       "1.2.3.4:1:2",
                                       "[::1]",
                                       "[::1]80",
                                       "::1:80",
                                       "[fe80::1%eth0]:80",
                                       "localhost:80",
                                       ""}) {
        const Result<Address> address = Address::parse(bad);
        ASSERT_FALSE(address.has_value()) << bad;
        EXPECT_EQ(address.error().code(), ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(Address::parse(std::string(65, '1')).has_value());
}

TEST(Address, EqualityCoversFamilyAndPort) {
    EXPECT_NE(Address::loopback_v4(1), Address::loopback_v4(2));
    EXPECT_NE(Address::v4({0, 0, 0, 0}, 5), Address::v6({}, 5));
    EXPECT_EQ(Address(), Address::v4({0, 0, 0, 0}, 0));
    EXPECT_EQ(Address::loopback_v4(1).bytes().size(), 4U);
    EXPECT_EQ(Address::loopback_v6(1).bytes().size(), 16U);
}

}  // namespace
}  // namespace orion::net
