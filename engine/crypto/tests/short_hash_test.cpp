#include "engine/crypto/short_hash.hpp"

#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace orion::crypto {
namespace {

class ShortHashTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(initialize().has_value()); }
};

// Khoá 00..0f, thông điệp 00..(n-1): vector của bản tham chiếu SipHash-2-4. Các số được tính lại
// bằng một bản cài Python độc lập, bản này cho đúng vector a129ca6149be45e5 của phụ lục A trong bài
// báo SipHash (Aumasson, Bernstein 2012) với n = 15.
TEST_F(ShortHashTest, MatchesReferenceVectors) {
    ShortHashKey key;
    for (usize i = 0; i < kShortHashKeySize; ++i) {
        key.mutable_view()[i] = static_cast<std::byte>(i);
    }
    std::array<std::byte, 64> message{};
    for (usize i = 0; i < message.size(); ++i) {
        message[i] = static_cast<std::byte>(i);
    }
    constexpr std::array<std::pair<usize, u64>, 7> kVectors = {{
        {0, 0x726FDB47DD0E0E31},
        {1, 0x74F839C593DC67FD},
        {7, 0xAB0200F58B01D137},
        {8, 0x93F5F5799A932462},
        {15, 0xA129CA6149BE45E5},
        {19, 0xBB6DC91DA77961BD},
        {63, 0x958A324CEB064572},
    }};
    for (const auto& [size, expected] : kVectors) {
        EXPECT_EQ(short_hash(std::span(message).first(size), key), expected) << size << " byte";
    }
}

TEST_F(ShortHashTest, DependsOnTheKey) {
    const ShortHashKey a = generate_short_hash_key();
    const ShortHashKey b = generate_short_hash_key();
    ASSERT_FALSE(equal_constant_time(a, b));
    const std::array data = {std::byte{192}, std::byte{0}, std::byte{2}, std::byte{1}};
    EXPECT_EQ(short_hash(data, a), short_hash(data, a));
    EXPECT_NE(short_hash(data, a), short_hash(data, b));
}

}  // namespace
}  // namespace orion::crypto
