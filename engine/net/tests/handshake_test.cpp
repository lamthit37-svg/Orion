// Gói bắt tay, chữ ký ACCEPT và cookie (docs/formats/transport.md, mục Bắt tay).

#include "engine/net/handshake.hpp"

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/packet.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace orion::net {
namespace {

using core::Duration;
using core::MonoTime;

template <usize N, class Tag>
[[nodiscard]] crypto::PublicBytes<N, Tag> filled(const u8 first) {
    crypto::PublicBytes<N, Tag> value;
    for (usize i = 0; i < N; ++i) {
        value.bytes[i] = static_cast<std::byte>(first + i);
    }
    return value;
}

[[nodiscard]] crypto::SigningKeyPair signing_key(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{fill});
    return crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
}

template <usize N, class Tag>
[[nodiscard]] bool same(const crypto::PublicBytes<N, Tag>& a,
                        const crypto::PublicBytes<N, Tag>& b) {
    return crypto::equal_constant_time(a, b);
}

[[nodiscard]] Request sample_request() {
    return {.protocol_version = 0x01020304,
            .nonce = filled<kHandshakeNonceSize, HandshakeNonceTag>(0x10),
            .token = filled<kConnectTokenSize, ConnectTokenTag>(0x40)};
}

[[nodiscard]] Accept sample_accept() {
    return {.nonce = filled<kHandshakeNonceSize, HandshakeNonceTag>(0x10),
            .connection_id = 0xA1B2C3D4,
            .server_key_exchange =
                filled<crypto::kKeyExchangePublicKeySize, crypto::KeyExchangePublicKeyTag>(0x60),
            .signature = filled<crypto::kSignatureSize, crypto::SignatureTag>(0x80)};
}

TEST(Handshake, SizesKeepEveryReplySmallerThanItsTrigger) {
    // Chống khuếch đại (X.9): mọi gói server gửi trước khi xác thực nhỏ hơn gói gây ra nó.
    EXPECT_LT(kChallengeSize, kRequestSize);
    EXPECT_LT(kRejectSize, kRequestSize);
    EXPECT_LT(kAcceptSize, kResponseSize);
    EXPECT_LT(kRejectSize, kResponseSize);
    EXPECT_EQ(kRequestSize, kMaxPacketSize);
}

TEST(Handshake, RequestLayoutAndRoundTrip) {
    const Request request = sample_request();
    const std::array<std::byte, kRequestSize> packet = write_request(request);
    EXPECT_EQ(packet[0], std::byte{0x01});
    EXPECT_EQ(core::load_le<u32>(std::span(packet).subspan<1, 4>()), 0x01020304U);
    EXPECT_EQ(packet[5], std::byte{0x10});
    EXPECT_EQ(packet[21], std::byte{0x40});
    EXPECT_EQ(packet[216], static_cast<std::byte>(0x40 + 195));
    EXPECT_TRUE(std::ranges::all_of(std::span(packet).subspan(217),
                                    [](const std::byte b) { return b == std::byte{0}; }));
    const Result<Request> read = read_request(packet);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->protocol_version, request.protocol_version);
    EXPECT_TRUE(same(read->nonce, request.nonce));
    EXPECT_TRUE(same(read->token, request.token));
}

TEST(Handshake, ChallengeResponseAcceptRejectRoundTrip) {
    const Challenge challenge{.nonce = filled<kHandshakeNonceSize, HandshakeNonceTag>(1),
                              .cookie = filled<kCookieSize, CookieTag>(100)};
    const std::array<std::byte, kChallengeSize> challenge_packet = write_challenge(challenge);
    EXPECT_EQ(challenge_packet[0], std::byte{0x02});
    EXPECT_EQ(challenge_packet[17], std::byte{100});
    const Result<Challenge> read_c = read_challenge(challenge_packet);
    ASSERT_TRUE(read_c.has_value());
    EXPECT_TRUE(same(read_c->nonce, challenge.nonce));
    EXPECT_TRUE(same(read_c->cookie, challenge.cookie));

    const Response response{.protocol_version = 7,
                            .nonce = challenge.nonce,
                            .cookie = challenge.cookie,
                            .token = filled<kConnectTokenSize, ConnectTokenTag>(9)};
    const std::array<std::byte, kResponseSize> response_packet = write_response(response);
    EXPECT_EQ(response_packet[0], std::byte{0x03});
    EXPECT_EQ(response_packet[21], std::byte{100});
    EXPECT_EQ(response_packet[53], std::byte{9});
    const Result<Response> read_r = read_response(response_packet);
    ASSERT_TRUE(read_r.has_value());
    EXPECT_EQ(read_r->protocol_version, 7U);
    EXPECT_TRUE(same(read_r->cookie, response.cookie));
    EXPECT_TRUE(same(read_r->token, response.token));

    const Accept accept = sample_accept();
    const std::array<std::byte, kAcceptSize> accept_packet = write_accept(accept);
    EXPECT_EQ(accept_packet[0], std::byte{0x04});
    EXPECT_EQ(core::load_le<u32>(std::span(accept_packet).subspan<17, 4>()), 0xA1B2C3D4U);
    EXPECT_EQ(accept_packet[21], std::byte{0x60});
    EXPECT_EQ(accept_packet[53], std::byte{0x80});
    const Result<Accept> read_a = read_accept(accept_packet);
    ASSERT_TRUE(read_a.has_value());
    EXPECT_EQ(read_a->connection_id, accept.connection_id);
    EXPECT_TRUE(same(read_a->server_key_exchange, accept.server_key_exchange));
    EXPECT_TRUE(same(read_a->signature, accept.signature));

    const Reject reject{.nonce = challenge.nonce, .reason = RejectReason::TokenExpired};
    const std::array<std::byte, kRejectSize> reject_packet = write_reject(reject);
    EXPECT_EQ(reject_packet[0], std::byte{0x05});
    EXPECT_EQ(reject_packet[17], std::byte{3});
    const Result<Reject> read_j = read_reject(reject_packet);
    ASSERT_TRUE(read_j.has_value());
    EXPECT_EQ(read_j->reason, RejectReason::TokenExpired);
    EXPECT_TRUE(same(read_j->nonce, reject.nonce));
}

TEST(Handshake, ReadersRejectWrongShape) {
    const std::array<std::byte, kRequestSize> request = write_request(sample_request());
    EXPECT_FALSE(read_request(std::span(request).first(kRequestSize - 1)).has_value());
    for (const usize padding : {usize{217}, usize{700}, kRequestSize - 1}) {
        std::array<std::byte, kRequestSize> dirty = request;
        dirty[padding] = std::byte{1};
        const Result<Request> read = read_request(dirty);
        ASSERT_FALSE(read.has_value()) << padding;
        EXPECT_EQ(read.error().code(), ErrorCode::InvalidArgument);
    }
    std::array<std::byte, kRequestSize> high_nibble = request;
    high_nibble[0] = std::byte{0x11};
    EXPECT_FALSE(read_request(high_nibble).has_value());
    // Gói của loại này không đọc được bằng hàm của loại khác.
    EXPECT_FALSE(read_challenge(std::span(request).first(kChallengeSize)).has_value());

    std::array<std::byte, kAcceptSize> accept = write_accept(sample_accept());
    std::fill_n(accept.begin() + 17, 4, std::byte{0});
    EXPECT_FALSE(read_accept(accept).has_value());
    std::vector<std::byte> longer(accept.begin(), accept.end());
    longer.push_back(std::byte{0});
    EXPECT_FALSE(read_accept(longer).has_value());

    std::array<std::byte, kRejectSize> reject =
        write_reject({.nonce = {}, .reason = RejectReason::ServerFull});
    for (const u8 reason : {u8{0}, u8{6}, u8{255}}) {
        reject[17] = std::byte{reason};
        EXPECT_FALSE(read_reject(reject).has_value()) << int{reason};
    }
    EXPECT_FALSE(read_response({}).has_value());
}

TEST(Handshake, AcceptSignatureBindsEveryField) {
    const crypto::SigningKeyPair identity = signing_key(0x33);
    const crypto::SigningKeyPair other = signing_key(0x44);
    const crypto::Hash hash = token_hash(sample_request().token);
    Accept accept = sample_accept();
    accept.signature = sign_accept(9, accept.nonce, hash, accept.connection_id,
                                   accept.server_key_exchange, identity.secret_key);
    EXPECT_TRUE(verify_accept(accept, 9, hash, identity.public_key));
    EXPECT_FALSE(verify_accept(accept, 10, hash, identity.public_key));
    EXPECT_FALSE(verify_accept(accept, 9, crypto::hash({}), identity.public_key));
    EXPECT_FALSE(verify_accept(accept, 9, hash, other.public_key));
    Accept changed_id = accept;
    changed_id.connection_id ^= 1U;
    EXPECT_FALSE(verify_accept(changed_id, 9, hash, identity.public_key));
    Accept changed_key = accept;
    changed_key.server_key_exchange.bytes[31] ^= std::byte{1};
    EXPECT_FALSE(verify_accept(changed_key, 9, hash, identity.public_key));
    Accept changed_nonce = accept;
    changed_nonce.nonce.bytes[0] ^= std::byte{1};
    EXPECT_FALSE(verify_accept(changed_nonce, 9, hash, identity.public_key));
    Accept changed_signature = accept;
    changed_signature.signature.bytes[10] ^= std::byte{1};
    EXPECT_FALSE(verify_accept(changed_signature, 9, hash, identity.public_key));
}

TEST(Handshake, TokenHashIsHashOfTheTokenBytes) {
    const ConnectToken token = sample_request().token;
    EXPECT_TRUE(same(token_hash(token), crypto::hash(token.view())));
}

class CookieJarTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(crypto::initialize().has_value()); }

    [[nodiscard]] static MonoTime at(const i64 seconds) {
        return MonoTime::from_nanoseconds(seconds * 1'000'000'000);
    }
    [[nodiscard]] Cookie issue(CookieJar& jar, const MonoTime now) const {
        return jar.issue(kVersion, nonce_, hash_, from_, now);
    }
    [[nodiscard]] bool check(const CookieJar& jar, const Cookie& cookie, const MonoTime now) const {
        return jar.check(cookie, kVersion, nonce_, hash_, from_, now);
    }

    [[nodiscard]] const HandshakeNonce& nonce() const noexcept { return nonce_; }
    [[nodiscard]] const crypto::Hash& hash() const noexcept { return hash_; }
    [[nodiscard]] const Address& from() const noexcept { return from_; }

    static constexpr u32 kVersion = 3;

private:
    HandshakeNonce nonce_ = filled<kHandshakeNonceSize, HandshakeNonceTag>(0x21);
    crypto::Hash hash_ = crypto::hash({});
    Address from_ = Address::v4({203, 0, 113, 7}, 40'000);
};

TEST_F(CookieJarTest, IssuedCookieChecksUntilItExpires) {
    CookieJar jar(at(100));
    const Cookie cookie = issue(jar, at(100));
    EXPECT_TRUE(check(jar, cookie, at(100)));
    EXPECT_TRUE(check(jar, cookie, at(100) + (kCookieLifetime - Duration::nanoseconds(1))));
    EXPECT_FALSE(check(jar, cookie, at(100) + kCookieLifetime));
}

TEST_F(CookieJarTest, CookieIsBoundToEveryField) {
    CookieJar jar(at(0));
    const Cookie cookie = issue(jar, at(0));
    const MonoTime now = at(1);
    EXPECT_FALSE(jar.check(cookie, kVersion + 1, nonce(), hash(), from(), now));
    HandshakeNonce other_nonce = nonce();
    other_nonce.bytes[15] ^= std::byte{1};
    EXPECT_FALSE(jar.check(cookie, kVersion, other_nonce, hash(), from(), now));
    EXPECT_FALSE(jar.check(cookie, kVersion, nonce(), crypto::hash(nonce().view()), from(), now));
    EXPECT_FALSE(
        jar.check(cookie, kVersion, nonce(), hash(), Address::v4({203, 0, 113, 7}, 40'001), now));
    EXPECT_FALSE(
        jar.check(cookie, kVersion, nonce(), hash(), Address::v4({203, 0, 113, 8}, 40'000), now));
    std::array<u8, 16> mapped{};
    mapped[0] = 203;
    mapped[2] = 113;
    mapped[3] = 7;
    EXPECT_FALSE(jar.check(cookie, kVersion, nonce(), hash(), Address::v6(mapped, 40'000), now));
    for (usize i = 0; i < kCookieSize; ++i) {
        Cookie tampered = cookie;
        tampered.bytes[i] ^= std::byte{0x01};
        EXPECT_FALSE(check(jar, tampered, now)) << "byte " << i;
    }
    // Jar khác (khoá khác) không nhận cookie này.
    const CookieJar other(at(0));
    EXPECT_FALSE(check(other, cookie, now));
}

TEST_F(CookieJarTest, EachCookieUsesAFreshSequence) {
    CookieJar jar(at(0));
    const Cookie first = issue(jar, at(0));
    const Cookie second = issue(jar, at(0));
    EXPECT_EQ(core::load_le<u64>(std::span(first.bytes).first<8>()), 0U);
    EXPECT_EQ(core::load_le<u64>(std::span(second.bytes).first<8>()), 1U);
    EXPECT_FALSE(same(first, second));
    EXPECT_TRUE(check(jar, first, at(0)));
    EXPECT_TRUE(check(jar, second, at(0)));
}

// Khoá thay mới mỗi kCookieKeyLifetime, khi cấp cookie: cookie của khoá trước còn được nhận tới lần
// thay sau (nếu chưa hết hạn), cookie của khoá trước nữa thì không.
TEST_F(CookieJarTest, RotationKeepsThePreviousKeyOnly) {
    const i64 period = kCookieKeyLifetime.as_seconds();
    CookieJar jar(at(0));
    const Cookie old = issue(jar, at(period - 1));
    const Cookie fresh = issue(jar, at(period));  // thay khoá
    EXPECT_EQ(core::load_le<u64>(std::span(fresh.bytes).first<8>()), 0U);
    EXPECT_TRUE(check(jar, old, at(period)));
    EXPECT_TRUE(check(jar, fresh, at(period)));
    static_cast<void>(issue(jar, at(2 * period)));  // thay lần nữa: khoá của `old` bị bỏ
    EXPECT_FALSE(check(jar, old, at(period)));
    EXPECT_TRUE(check(jar, fresh, at(period)));
}

}  // namespace
}  // namespace orion::net
