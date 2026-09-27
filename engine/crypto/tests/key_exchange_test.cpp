#include "engine/crypto/key_exchange.hpp"

#include "engine/core/error.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/tests/support/hex.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace orion::crypto {
namespace {

using testing::fixed_from_hex;
using testing::secret_from_hex;

class KeyExchangeTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(initialize().has_value()); }
};

// RFC 7748 §6.1: Alice làm client, Bob làm server.
constexpr std::string_view kAliceSecret =
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
constexpr std::string_view kAlicePublic =
    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
constexpr std::string_view kBobSecret =
    "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
constexpr std::string_view kBobPublic =
    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
// BLAKE2b-512(bí mật chung || khoá công khai client || khoá công khai server), chia đôi: nửa đầu
// là chiều nhận của client. Tính bằng hashlib và X25519 của gói cryptography (Python).
constexpr std::string_view kClientReceive =
    "322b7be3b9bce4a84fe6e2dea61e8e6d0a98f3e4c60b58bad722b1c855c9db22";
constexpr std::string_view kClientTransmit =
    "284901a611708379d0b5b0e40d77ea207624eaab8dd0c95e693fc3ee76c73ccb";

TEST_F(KeyExchangeTest, PublicKeysMatchRfc7748) {
    const KeyExchangeKeyPair alice =
        key_exchange_key_pair_from_secret(secret_from_hex<KeyExchangeSecretKey>(kAliceSecret));
    const KeyExchangeKeyPair bob =
        key_exchange_key_pair_from_secret(secret_from_hex<KeyExchangeSecretKey>(kBobSecret));
    EXPECT_TRUE(
        equal_constant_time(alice.public_key, fixed_from_hex<KeyExchangePublicKey>(kAlicePublic)));
    EXPECT_TRUE(
        equal_constant_time(bob.public_key, fixed_from_hex<KeyExchangePublicKey>(kBobPublic)));
}

TEST_F(KeyExchangeTest, SessionKeysMatchKnownDerivation) {
    const KeyExchangeKeyPair alice =
        key_exchange_key_pair_from_secret(secret_from_hex<KeyExchangeSecretKey>(kAliceSecret));
    const KeyExchangeKeyPair bob =
        key_exchange_key_pair_from_secret(secret_from_hex<KeyExchangeSecretKey>(kBobSecret));
    const Result<SessionKeys> client = client_session_keys(alice, bob.public_key);
    const Result<SessionKeys> server = server_session_keys(bob, alice.public_key);
    ASSERT_TRUE(client.has_value());
    ASSERT_TRUE(server.has_value());
    EXPECT_TRUE(equal_constant_time(client->receive, secret_from_hex<AeadKey>(kClientReceive)));
    EXPECT_TRUE(equal_constant_time(client->transmit, secret_from_hex<AeadKey>(kClientTransmit)));
    EXPECT_TRUE(equal_constant_time(server->transmit, client->receive));
    EXPECT_TRUE(equal_constant_time(server->receive, client->transmit));
}

TEST_F(KeyExchangeTest, GeneratedPairsAgree) {
    const KeyExchangeKeyPair client = generate_key_exchange_key_pair();
    const KeyExchangeKeyPair server = generate_key_exchange_key_pair();
    const KeyExchangeKeyPair recomputed = key_exchange_key_pair_from_secret(client.secret_key);
    EXPECT_TRUE(equal_constant_time(recomputed.public_key, client.public_key));
    const Result<SessionKeys> client_keys = client_session_keys(client, server.public_key);
    const Result<SessionKeys> server_keys = server_session_keys(server, client.public_key);
    ASSERT_TRUE(client_keys.has_value());
    ASSERT_TRUE(server_keys.has_value());
    EXPECT_TRUE(equal_constant_time(client_keys->receive, server_keys->transmit));
    EXPECT_TRUE(equal_constant_time(client_keys->transmit, server_keys->receive));
    // Hai chiều dùng hai khoá khác nhau, nên số thứ tự của hai chiều không đụng nhau.
    EXPECT_FALSE(equal_constant_time(client_keys->receive, client_keys->transmit));
}

// Khoá công khai bậc thấp làm bí mật chung toàn 0: kẻ tấn công ép được khoá phiên đoán trước.
TEST_F(KeyExchangeTest, RejectsLowOrderPublicKeys) {
    const KeyExchangeKeyPair client = generate_key_exchange_key_pair();
    const KeyExchangeKeyPair server = generate_key_exchange_key_pair();
    // u = 0 (bậc 2), u = 1 (bậc 4), và một điểm bậc 8 (kiểm bằng ladder Montgomery trong Python).
    for (const std::string_view hex :
         {"0000000000000000000000000000000000000000000000000000000000000000",
          "0100000000000000000000000000000000000000000000000000000000000000",
          "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"}) {
        SCOPED_TRACE(hex);
        const auto low_order = fixed_from_hex<KeyExchangePublicKey>(hex);
        const Result<SessionKeys> as_client = client_session_keys(client, low_order);
        ASSERT_FALSE(as_client.has_value());
        EXPECT_EQ(as_client.error().code(), ErrorCode::InvalidArgument);
        const Result<SessionKeys> as_server = server_session_keys(server, low_order);
        ASSERT_FALSE(as_server.has_value());
        EXPECT_EQ(as_server.error().code(), ErrorCode::InvalidArgument);
    }
}

}  // namespace
}  // namespace orion::crypto
