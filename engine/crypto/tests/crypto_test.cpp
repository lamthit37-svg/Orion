#include "engine/crypto/crypto.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace orion::crypto {
namespace {

class Crypto : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(initialize().has_value()); }
};

TEST_F(Crypto, InitializeIsIdempotent) {
    EXPECT_TRUE(initialize().has_value());
    EXPECT_TRUE(initialize().has_value());
}

TEST_F(Crypto, RandomBytesAreNotAllZeroAndDiffer) {
    std::array<std::byte, 32> a{};
    std::array<std::byte, 32> b{};
    random_bytes(a);
    random_bytes(b);
    // Xác suất 32 byte ngẫu nhiên trùng nhau hoặc toàn 0 là 2^-256.
    EXPECT_FALSE(equal_constant_time(a, b));
    EXPECT_FALSE(equal_constant_time(a, std::array<std::byte, 32>{}));
}

TEST_F(Crypto, ConstantTimeEqualityComparesContentAndLength) {
    const std::array<std::byte, 3> x{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> y{std::byte{1}, std::byte{2}, std::byte{4}};
    const std::array<std::byte, 2> shorter{std::byte{1}, std::byte{2}};
    EXPECT_TRUE(equal_constant_time(x, x));
    EXPECT_FALSE(equal_constant_time(x, y));
    EXPECT_FALSE(equal_constant_time(x, shorter));
    EXPECT_TRUE(equal_constant_time(std::span<const std::byte>{}, std::span<const std::byte>{}));
}

TEST_F(Crypto, SecureZeroClearsMemory) {
    std::array<std::byte, 16> buffer{};
    buffer.fill(std::byte{0xAB});
    secure_zero(buffer);
    EXPECT_TRUE(equal_constant_time(buffer, std::array<std::byte, 16>{}));
}

struct TestSecretTag;
using TestSecret = SecretBytes<8, TestSecretTag>;

TEST_F(Crypto, SecretBytesAreWipedWhenMovedFrom) {
    TestSecret source;
    source.mutable_view()[0] = std::byte{0x42};
    const TestSecret copy = source;
    EXPECT_TRUE(equal_constant_time(copy, source));
    TestSecret moved(std::move(source));
    EXPECT_EQ(moved.view()[0], std::byte{0x42});
    // Test cố ý đọc đối tượng đã bị move để kiểm nó đã bị xoá.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(source.view()[0], std::byte{0});
    TestSecret assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.view()[0], std::byte{0x42});
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(moved.view()[0], std::byte{0});
}

}  // namespace
}  // namespace orion::crypto
