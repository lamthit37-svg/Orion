// Tệp khoá và tệp bí mật (docs/formats/service.md, mục "Khoá và bí mật").

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "game/server/lib/service/detail/secret.hpp"
#include "game/server/lib/service/service.hpp"
#include "game/server/lib/service/tests/support/files.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace orion::service {
namespace {

using io::testing::TempPath;

// Seed 00 01 02 … 1f và dạng hex của nó.
constexpr std::string_view kSeedHex =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

[[nodiscard]] crypto::SigningSeed seed() {
    std::array<std::byte, crypto::kSigningSeedSize> bytes{};
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes.at(i) = static_cast<std::byte>(i);
    }
    return crypto::SigningSeed(std::span<const std::byte, crypto::kSigningSeedSize>(bytes));
}

template <class T>
[[nodiscard]] std::optional<ErrorCode> error_of(const Result<T>& result) {
    if (result) {
        return std::nullopt;
    }
    return result.error().code();
}

class KeyFileTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(crypto::initialize().has_value()); }

    [[nodiscard]] const TempPath& key() const noexcept { return key_; }
    [[nodiscard]] const TempPath& dev_key() const noexcept { return dev_key_; }

private:
    TempPath key_{"auth.key"};
    TempPath dev_key_{"auth.dev.key"};
};

TEST_F(KeyFileTest, SigningKeyIsTheSeedInHex) {
    const crypto::SigningKeyPair expected = crypto::signing_key_pair_from_seed(seed());
    for (const std::string& text :
         {std::string(kSeedHex), std::string(kSeedHex) + "\n", std::string(kSeedHex) + "\r\n",
          std::string("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F")}) {
        testing::write_file(key().path(), text);
        const Result<crypto::SigningKeyPair> pair =
            load_signing_key(key().path(), Environment::Production);
        ASSERT_TRUE(pair.has_value()) << text;
        EXPECT_EQ(pair->public_key.bytes, expected.public_key.bytes) << text;
    }
}

TEST_F(KeyFileTest, PublicKeyIsItsBytesInHex) {
    testing::write_file(key().path(), std::string(kSeedHex) + "\n");
    const Result<crypto::SigningPublicKey> loaded =
        load_public_key(key().path(), Environment::Production);
    ASSERT_TRUE(loaded.has_value());
    for (usize i = 0; i < loaded->bytes.size(); ++i) {
        EXPECT_EQ(loaded->bytes.at(i), static_cast<std::byte>(i)) << i;
    }
}

TEST_F(KeyFileTest, RejectsMalformedKeyFiles) {
    const std::string short_key(kSeedHex.substr(2));
    const std::string long_key = std::string(kSeedHex) + "00";
    std::string not_hex(kSeedHex);
    not_hex[10] = 'g';
    std::string inner_space(kSeedHex);
    inner_space[31] = ' ';
    for (const std::string& text :
         {std::string(), std::string("\n"), short_key, long_key, not_hex, inner_space,
          std::string(kSeedHex) + "\n\n", " " + std::string(kSeedHex)}) {
        testing::write_file(key().path(), text);
        EXPECT_EQ(error_of(load_signing_key(key().path(), Environment::Local)),
                  ErrorCode::InvalidArgument)
            << text;
        EXPECT_EQ(error_of(load_public_key(key().path(), Environment::Local)),
                  ErrorCode::InvalidArgument)
            << text;
    }
}

TEST_F(KeyFileTest, RejectsKeyFilesOverTheSecretLimit) {
    testing::write_file(key().path(), std::string(kMaxSecretFileBytes + 1, '0'));
    EXPECT_EQ(error_of(load_signing_key(key().path(), Environment::Local)), ErrorCode::OutOfRange);
}

TEST_F(KeyFileTest, MissingKeyFileIsNotFound) {
    EXPECT_EQ(error_of(load_signing_key(key().path(), Environment::Local)), ErrorCode::NotFound);
}

TEST_F(KeyFileTest, DevKeysLoadOnlyInLocal) {
    testing::write_file(dev_key().path(), kSeedHex);
    EXPECT_EQ(error_of(load_signing_key(dev_key().path(), Environment::Production)),
              ErrorCode::FailedPrecondition);
    EXPECT_EQ(error_of(load_public_key(dev_key().path(), Environment::Production)),
              ErrorCode::FailedPrecondition);
    // Bản ship từ chối khoá dev ở mọi môi trường (X.9).
    EXPECT_EQ(load_signing_key(dev_key().path(), Environment::Local).has_value(), ORION_SHIP == 0);
    EXPECT_EQ(load_public_key(dev_key().path(), Environment::Local).has_value(), ORION_SHIP == 0);
}

TEST(DevSecret, IsDecidedByTheFileNameOnly) {
    EXPECT_TRUE(detail::is_dev_secret("auth.dev.key"));
    EXPECT_TRUE(detail::is_dev_secret("deploy/local/auth.dev.key"));
    EXPECT_TRUE(detail::is_dev_secret("C:\\orion\\keys\\conninfo.dev.txt"));
    EXPECT_TRUE(detail::is_dev_secret(".dev."));
    EXPECT_FALSE(detail::is_dev_secret("auth.key"));
    EXPECT_FALSE(detail::is_dev_secret("auth.dev"));
    EXPECT_FALSE(detail::is_dev_secret("dev.auth.key"));
    EXPECT_FALSE(detail::is_dev_secret("auth.development.key"));
    // Thư mục có ".dev." không làm tệp thành bí mật dev.
    EXPECT_FALSE(detail::is_dev_secret("/srv/keys.dev.d/auth.key"));
    EXPECT_FALSE(detail::is_dev_secret("keys.dev.d\\auth.key"));
}

TEST(DevSecret, AllowedOnlyInLocalAndNeverInShip) {
    EXPECT_TRUE(detail::allow_secret("auth.key", Environment::Production).has_value());
    EXPECT_TRUE(detail::allow_secret("auth.key", Environment::Local).has_value());
    EXPECT_EQ(error_of(detail::allow_secret("auth.dev.key", Environment::Production)),
              ErrorCode::FailedPrecondition);
    EXPECT_EQ(detail::allow_secret("auth.dev.key", Environment::Local).has_value(),
              ORION_SHIP == 0);
}

TEST(SecretWipe, EmptiesTheString) {
    std::string text(64, 'k');
    text.resize(10);
    detail::wipe(text);
    EXPECT_TRUE(text.empty());
}

}  // namespace
}  // namespace orion::service
