#include "engine/crypto/password.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace orion::crypto {
namespace {

class PasswordTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(initialize().has_value()); }
};

// Sinh bằng argon2-cffi 25.1.0 (bản cài đặt tham chiếu của Argon2, không phải libsodium), salt
// "orion-test-salt!": kiểm libsodium đọc được chuỗi của một bản cài đặt khác.
constexpr std::string_view kReferenceHash =
    "$argon2id$v=19$m=8,t=1,p=1$b3Jpb24tdGVzdC1zYWx0IQ$Ikmo4c04l9W9y6vK60aQBO2rb14U84yH8I3cHkOrgTo";
constexpr std::string_view kReferencePassword = "correct horse battery staple";
constexpr std::string_view kReferenceEmptyHash =
    "$argon2id$v=19$m=8,t=1,p=1$b3Jpb24tdGVzdC1zYWx0IQ$pzkqMQSo/HyvvvewpIR8CEYa6NPZkMnZ8C7tsoS47IE";
constexpr std::string_view kReferenceUtf8Hash =
    "$argon2id$v=19$m=64,t=2,p=1$b3Jpb24tdGVzdC1zYWx0IQ$"
    "CpvN65FdOaZrQz2hPI0lmTgAl8B2iTdD8OOtOV7fmlk";

TEST_F(PasswordTest, VerifiesHashesFromReferenceImplementation) {
    const Result<PasswordHash> hash = PasswordHash::parse(kReferenceHash);
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password(*hash, kReferencePassword));
    EXPECT_FALSE(verify_password(*hash, "correct horse battery stapler"));
    EXPECT_FALSE(verify_password(*hash, ""));

    const Result<PasswordHash> empty = PasswordHash::parse(kReferenceEmptyHash);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(verify_password(*empty, ""));

    const Result<PasswordHash> utf8 = PasswordHash::parse(kReferenceUtf8Hash);
    ASSERT_TRUE(utf8.has_value());
    EXPECT_TRUE(verify_password(*utf8, "mật khẩu"));
    EXPECT_FALSE(verify_password(*utf8, "mat khau"));
}

TEST_F(PasswordTest, HashRoundTripsThroughParse) {
    const Result<PasswordHash> hash = hash_password("hunter2", minimum_password_limits());
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(hash->view().starts_with("$argon2id$v=19$m=8,t=1,p=1$"));
    EXPECT_TRUE(verify_password(*hash, "hunter2"));
    EXPECT_FALSE(verify_password(*hash, "hunter3"));

    const Result<PasswordHash> reparsed = PasswordHash::parse(hash->view());
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_EQ(reparsed->view(), hash->view());
    EXPECT_TRUE(verify_password(*reparsed, "hunter2"));
}

TEST_F(PasswordTest, SaltDiffersEveryTime) {
    const Result<PasswordHash> first = hash_password("same", minimum_password_limits());
    const Result<PasswordHash> second = hash_password("same", minimum_password_limits());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->view(), second->view());
    EXPECT_TRUE(verify_password(*second, "same"));
}

// Mức mặc định của dịch vụ auth: 64 MiB, 2 lượt.
TEST_F(PasswordTest, InteractiveLimitsHashAndNeedNoRehash) {
    const PasswordLimits interactive = interactive_password_limits();
    EXPECT_EQ(interactive.operations, 2U);
    EXPECT_EQ(interactive.memory_bytes, usize{64} * 1024 * 1024);
    const Result<PasswordHash> hash = hash_password("p@ss", interactive);
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(hash->view().starts_with("$argon2id$v=19$m=65536,t=2,p=1$"));
    EXPECT_TRUE(verify_password(*hash, "p@ss"));
    EXPECT_FALSE(password_needs_rehash(*hash, interactive));
    EXPECT_TRUE(password_needs_rehash(*hash, minimum_password_limits()));
}

TEST_F(PasswordTest, NeedsRehashWhenParametersChange) {
    const Result<PasswordHash> hash = PasswordHash::parse(kReferenceHash);
    ASSERT_TRUE(hash.has_value());
    EXPECT_FALSE(password_needs_rehash(*hash, minimum_password_limits()));
    EXPECT_TRUE(password_needs_rehash(*hash, {.operations = 2, .memory_bytes = usize{8} * 1024}));
    EXPECT_TRUE(password_needs_rehash(*hash, {.operations = 1, .memory_bytes = usize{16} * 1024}));
}

TEST_F(PasswordTest, PasswordLengthLimit) {
    const std::string longest(kMaxPasswordBytes, 'a');
    const Result<PasswordHash> hash = hash_password(longest, minimum_password_limits());
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(verify_password(*hash, longest));

    const std::string too_long(kMaxPasswordBytes + 1, 'a');
    const Result<PasswordHash> rejected = hash_password(too_long, minimum_password_limits());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(verify_password(*hash, too_long));
}

TEST_F(PasswordTest, RejectsLimitsParseWouldNotAccept) {
    constexpr usize kMinimumBytes = usize{8} * 1024;
    const std::array<PasswordLimits, 4> invalid{{
        {.operations = 0, .memory_bytes = kMinimumBytes},
        {.operations = kMaxPasswordOperations + 1, .memory_bytes = kMinimumBytes},
        {.operations = 1, .memory_bytes = kMinimumBytes - 1},
        {.operations = 1, .memory_bytes = (kMaxPasswordMemoryKib + 1) * 1024},
    }};
    for (const PasswordLimits& limits : invalid) {
        SCOPED_TRACE(limits.operations);
        SCOPED_TRACE(limits.memory_bytes);
        const Result<PasswordHash> hash = hash_password("x", limits);
        ASSERT_FALSE(hash.has_value());
        EXPECT_EQ(hash.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST_F(PasswordTest, ParseAcceptsParameterBounds) {
    const std::string salt_and_hash =
        "$b3Jpb24tdGVzdC1zYWx0IQ$Ikmo4c04l9W9y6vK60aQBO2rb14U84yH8I3cHkOrgTo";
    EXPECT_TRUE(PasswordHash::parse("$argon2id$v=19$m=8,t=1,p=1" + salt_and_hash).has_value());
    EXPECT_TRUE(
        PasswordHash::parse("$argon2id$v=19$m=1048576,t=10,p=1" + salt_and_hash).has_value());
}

// Chuỗi đọc từ DB là dữ liệu từ ngoài (X.5): mọi dạng lệch đều là InvalidArgument trước khi tới
// libsodium, nhất là tham số làm libsodium cấp bộ nhớ hay chạy quá lâu.
TEST_F(PasswordTest, ParseRejectsMalformedStrings) {
    const std::string salt = "b3Jpb24tdGVzdC1zYWx0IQ";
    const std::string digest = "Ikmo4c04l9W9y6vK60aQBO2rb14U84yH8I3cHkOrgTo";
    const std::string tail = "$" + salt + "$" + digest;
    const std::vector<std::string> rejected{
        "",
        "$argon2id$",
        "$argon2i$v=19$m=8,t=1,p=1" + tail,
        "$argon2d$v=19$m=8,t=1,p=1" + tail,
        "$argon2id$v=16$m=8,t=1,p=1" + tail,
        "$argon2id$m=8,t=1,p=1" + tail,
        "$argon2id$v=19$m=0,t=1,p=1" + tail,
        "$argon2id$v=19$m=7,t=1,p=1" + tail,
        "$argon2id$v=19$m=1048577,t=1,p=1" + tail,
        "$argon2id$v=19$m=4294967295,t=1,p=1" + tail,
        "$argon2id$v=19$m=99999999999999999999999,t=1,p=1" + tail,
        "$argon2id$v=19$m=08,t=1,p=1" + tail,
        "$argon2id$v=19$m=+8,t=1,p=1" + tail,
        "$argon2id$v=19$m=-8,t=1,p=1" + tail,
        "$argon2id$v=19$m=,t=1,p=1" + tail,
        "$argon2id$v=19$m=8,t=0,p=1" + tail,
        "$argon2id$v=19$m=8,t=11,p=1" + tail,
        "$argon2id$v=19$m=8,t=4294967295,p=1" + tail,
        "$argon2id$v=19$m=8,t=1,p=2" + tail,
        "$argon2id$v=19$m=8,t=1,p=0" + tail,
        "$argon2id$v=19$t=1,m=8,p=1" + tail,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt.substr(1) + "$" + digest,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "A$" + digest,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "$" + digest.substr(1),
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "$" + digest + "A",
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "$" + digest + "$",
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "$" + digest + " ",
        "$argon2id$v=19$m=8,t=1,p=1$" + salt,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "$",
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + digest,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt.substr(0, 21) + "-$" + digest,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt.substr(0, 21) + "_$" + digest,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt.substr(0, 21) + "=$" + digest,
        "$argon2id$v=19$m=8,t=1,p=1$" + salt + "$" + digest.substr(0, 42) + "=",
        " $argon2id$v=19$m=8,t=1,p=1" + tail,
        std::string(kPasswordHashCapacity, 'a'),
        std::string(1 << 16, '$'),
    };
    for (const std::string& text : rejected) {
        SCOPED_TRACE(text.substr(0, 120));
        const Result<PasswordHash> hash = PasswordHash::parse(text);
        ASSERT_FALSE(hash.has_value());
        EXPECT_EQ(hash.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST_F(PasswordTest, ParseRejectsEmbeddedNul) {
    std::string text = std::string(kReferenceHash);
    text[30] = '\0';
    EXPECT_FALSE(PasswordHash::parse(text).has_value());
    text = std::string(kReferenceHash) + '\0';
    EXPECT_FALSE(PasswordHash::parse(text).has_value());
}

// Dạng đúng nhưng hash không phải của mật khẩu nào đã biết: chỉ cho false.
TEST_F(PasswordTest, ForgedButWellFormedHashOnlyFailsVerification) {
    const Result<PasswordHash> forged = PasswordHash::parse(
        "$argon2id$v=19$m=8,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    ASSERT_TRUE(forged.has_value());
    EXPECT_FALSE(verify_password(*forged, ""));
    EXPECT_FALSE(verify_password(*forged, "x"));
    // Bit thừa ở ký tự base64 cuối khác 0: libsodium từ chối khi giải mã, vẫn chỉ là false.
    const Result<PasswordHash> noncanonical = PasswordHash::parse(
        "$argon2id$v=19$m=8,t=1,p=1$b3Jpb24tdGVzdC1zYWx0IR$"
        "Ikmo4c04l9W9y6vK60aQBO2rb14U84yH8I3cHkOrgTo");
    ASSERT_TRUE(noncanonical.has_value());
    EXPECT_FALSE(verify_password(*noncanonical, kReferencePassword));
    EXPECT_TRUE(password_needs_rehash(*noncanonical, minimum_password_limits()));
}

}  // namespace
}  // namespace orion::crypto
