#include "engine/crypto/hash.hpp"

#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/tests/support/hex.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

namespace orion::crypto {
namespace {

using testing::as_byte_vector;
using testing::fixed_from_hex;

class HashTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(initialize().has_value()); }
};

// BLAKE2b 256 bit, đối chiếu với hashlib.blake2b(digest_size=32) của Python.
TEST_F(HashTest, MatchesKnownDigests) {
    EXPECT_TRUE(equal_constant_time(
        hash({}),
        fixed_from_hex<Hash>("0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8")));
    EXPECT_TRUE(equal_constant_time(
        hash(as_byte_vector("abc")),
        fixed_from_hex<Hash>("bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319")));
}

// 256 000 byte (0..255 lặp lại) băm một lần và băm theo khối lệch nhau cho cùng kết quả.
TEST_F(HashTest, StreamingMatchesOneShot) {
    std::vector<std::byte> data(256'000);
    for (usize i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i & 0xFFU);
    }
    const Hash expected =
        fixed_from_hex<Hash>("5d074c3f889bcba3b3a253496ee73cba18832b6d3c67d2b739dc202c78ed2581");
    EXPECT_TRUE(equal_constant_time(hash(data), expected));
    Hasher hasher;
    const std::span<const std::byte> all = data;
    usize offset = 0;
    for (const usize chunk : {1U, 127U, 128U, 129U, 4'096U, 65'536U}) {
        hasher.update(all.subspan(offset, chunk));
        offset += chunk;
    }
    hasher.update(all.subspan(offset));
    EXPECT_TRUE(equal_constant_time(hasher.finish(), expected));
}

TEST_F(HashTest, EmptyUpdatesChangeNothing) {
    Hasher hasher;
    hasher.update({});
    hasher.update(as_byte_vector("abc"));
    hasher.update({});
    EXPECT_TRUE(equal_constant_time(hasher.finish(), hash(as_byte_vector("abc"))));
}

class HashDeathTest : public HashTest {
protected:
    void SetUp() override {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        HashTest::SetUp();
    }
};

// libsodium trả lỗi khi finish lần hai; với Hasher đó là lỗi lập trình của bên gọi.
TEST_F(HashDeathTest, FinishTwiceIsProgrammingError) {
    Hasher hasher;
    hasher.update(as_byte_vector("abc"));
    static_cast<void>(hasher.finish());
    EXPECT_DEATH(static_cast<void>(hasher.finish()), "Hasher::finish gọi hai lần");
}

}  // namespace
}  // namespace orion::crypto
