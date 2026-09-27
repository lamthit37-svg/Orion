#include "engine/crypto/aead.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/tests/support/hex.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace orion::crypto {
namespace {

using testing::as_byte_vector;
using testing::fixed_from_hex;
using testing::from_hex;

class AeadTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(initialize().has_value()); }
};

[[nodiscard]] AeadKey rfc8439_key() {
    AeadKey key;
    const std::span<std::byte, kAeadKeySize> bytes = key.mutable_view();
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(0x80 + i);
    }
    return key;
}

constexpr std::string_view kRfc8439Plaintext =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
    "sunscreen would be it.";
constexpr std::string_view kRfc8439AssociatedData = "50515253c0c1c2c3c4c5c6c7";
constexpr std::string_view kRfc8439Ciphertext =
    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
    "3ff4def08e4b7a9de576d26586cec64b6116"
    "1ae10b594f09e26a7e902ecbd0600691";

// Nonce của RFC 8439 §2.8.2 là 07 00 00 00 rồi IV 40..47: đúng bố cục channel (u32) rồi sequence
// (u64) little-endian của nonce_from_sequence.
TEST_F(AeadTest, NonceLayoutMatchesRfc8439) {
    const AeadNonce nonce = nonce_from_sequence(7, 0x4746'4544'4342'4140ULL);
    EXPECT_TRUE(equal_constant_time(nonce, fixed_from_hex<AeadNonce>("070000004041424344454647")));
}

TEST_F(AeadTest, SealMatchesRfc8439Vector) {
    const std::vector<std::byte> plaintext = as_byte_vector(kRfc8439Plaintext);
    const std::vector<std::byte> associated_data = from_hex(kRfc8439AssociatedData);
    const std::vector<std::byte> expected = from_hex(kRfc8439Ciphertext);
    const AeadNonce nonce = nonce_from_sequence(7, 0x4746'4544'4342'4140ULL);
    std::vector<std::byte> sealed(plaintext.size() + kAeadTagSize);
    const Result<usize> written = seal(sealed, plaintext, associated_data, nonce, rfc8439_key());
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, expected.size());
    EXPECT_TRUE(equal_constant_time(sealed, expected));

    std::vector<std::byte> opened(plaintext.size());
    const Result<usize> read = open(opened, expected, associated_data, nonce, rfc8439_key());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, plaintext.size());
    EXPECT_TRUE(equal_constant_time(opened, plaintext));
}

// Mọi byte của gói, associated data, nonce hay khoá đổi thì open trả DataLoss và out bị ghi 0.
TEST_F(AeadTest, AnyTamperingFailsAuthentication) {
    const AeadKey key = generate_aead_key();
    const AeadNonce nonce = nonce_from_sequence(1, 42);
    const std::vector<std::byte> plaintext = as_byte_vector("move 12 -4");
    const std::vector<std::byte> header = as_byte_vector("hdr");
    std::vector<std::byte> sealed(plaintext.size() + kAeadTagSize);
    ASSERT_TRUE(seal(sealed, plaintext, header, nonce, key).has_value());

    std::vector<std::byte> out(plaintext.size(), std::byte{0x5A});
    const auto expect_rejected = [&](const Result<usize>& result) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::DataLoss);
        EXPECT_TRUE(std::ranges::all_of(out, [](const std::byte b) { return b == std::byte{0}; }));
        std::ranges::fill(out, std::byte{0x5A});
    };
    for (usize i = 0; i < sealed.size(); ++i) {
        SCOPED_TRACE(i);
        std::vector<std::byte> altered = sealed;
        altered[i] ^= std::byte{0x01};
        expect_rejected(open(out, altered, header, nonce, key));
    }
    expect_rejected(open(out, sealed, as_byte_vector("hdR"), nonce, key));
    expect_rejected(open(out, sealed, {}, nonce, key));
    expect_rejected(open(out, sealed, header, nonce_from_sequence(1, 43), key));
    expect_rejected(open(out, sealed, header, nonce_from_sequence(2, 42), key));
    expect_rejected(open(out, sealed, header, nonce, generate_aead_key()));
}

TEST_F(AeadTest, ShortPacketsAreDataLossWithoutTouchingOut) {
    const AeadKey key = generate_aead_key();
    std::vector<std::byte> out(4, std::byte{0x5A});
    for (const usize size : {usize{0}, usize{1}, kAeadTagSize - 1}) {
        const std::vector<std::byte> packet(size, std::byte{0x11});
        const Result<usize> result = open(out, packet, {}, nonce_from_sequence(0, 0), key);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::DataLoss);
    }
    EXPECT_TRUE(std::ranges::all_of(out, [](const std::byte b) { return b == std::byte{0x5A}; }));
}

TEST_F(AeadTest, EmptyPlaintextStillAuthenticates) {
    const AeadKey key = generate_aead_key();
    const AeadNonce nonce = nonce_from_sequence(3, 0);
    const std::vector<std::byte> header = as_byte_vector("ack");
    std::vector<std::byte> sealed(kAeadTagSize);
    const Result<usize> written = seal(sealed, {}, header, nonce, key);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, kAeadTagSize);
    const Result<usize> read = open({}, sealed, header, nonce, key);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 0U);
    sealed[0] ^= std::byte{0x01};
    EXPECT_FALSE(open({}, sealed, header, nonce, key).has_value());
}

TEST_F(AeadTest, InPlaceSealAndOpen) {
    const AeadKey key = generate_aead_key();
    const AeadNonce nonce = nonce_from_sequence(0, 7);
    const std::vector<std::byte> plaintext = as_byte_vector("snapshot delta 0123456789abcdef");
    std::vector<std::byte> buffer = plaintext;
    buffer.resize(plaintext.size() + kAeadTagSize);
    const std::span<std::byte> message = std::span(buffer).first(plaintext.size());
    const Result<usize> written = seal(buffer, message, {}, nonce, key);
    ASSERT_TRUE(written.has_value());
    EXPECT_FALSE(equal_constant_time(message, plaintext));
    const Result<usize> read = open(buffer, buffer, {}, nonce, key);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(equal_constant_time(std::span(buffer).first(*read), plaintext));
}

TEST_F(AeadTest, OutputBuffersTooSmallAreInvalidArgument) {
    const AeadKey key = generate_aead_key();
    const AeadNonce nonce = nonce_from_sequence(0, 1);
    const std::vector<std::byte> plaintext(10, std::byte{1});
    std::vector<std::byte> small(plaintext.size() + kAeadTagSize - 1);
    const Result<usize> sealed_small = seal(small, plaintext, {}, nonce, key);
    ASSERT_FALSE(sealed_small.has_value());
    EXPECT_EQ(sealed_small.error().code(), ErrorCode::InvalidArgument);

    std::vector<std::byte> sealed(plaintext.size() + kAeadTagSize);
    ASSERT_TRUE(seal(sealed, plaintext, {}, nonce, key).has_value());
    std::vector<std::byte> out(plaintext.size() - 1);
    const Result<usize> opened_small = open(out, sealed, {}, nonce, key);
    ASSERT_FALSE(opened_small.has_value());
    EXPECT_EQ(opened_small.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(AeadTest, SameInputsSealIdentically) {
    const AeadKey key = generate_aead_key();
    const std::vector<std::byte> plaintext = as_byte_vector("deterministic");
    std::vector<std::byte> first(plaintext.size() + kAeadTagSize);
    std::vector<std::byte> second(first.size());
    std::vector<std::byte> next_sequence(first.size());
    ASSERT_TRUE(seal(first, plaintext, {}, nonce_from_sequence(0, 9), key).has_value());
    ASSERT_TRUE(seal(second, plaintext, {}, nonce_from_sequence(0, 9), key).has_value());
    ASSERT_TRUE(seal(next_sequence, plaintext, {}, nonce_from_sequence(0, 10), key).has_value());
    EXPECT_TRUE(equal_constant_time(first, second));
    EXPECT_FALSE(equal_constant_time(first, next_sequence));
}

}  // namespace
}  // namespace orion::crypto
