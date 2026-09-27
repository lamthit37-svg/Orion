// Client và server của transport trên một mạng giả tất định (docs/formats/transport.md, mục Bắt tay
// và Kết nối): gói đi qua hai Outbox, thời gian là đồng hồ giả, không có socket nào (X.4).

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/short_hash.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/client_transport.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/connection.hpp"
#include "engine/net/handshake.hpp"
#include "engine/net/outbox.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/rate_limit.hpp"
#include "engine/net/secure_channel.hpp"
#include "engine/net/server_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::net {
namespace {

using core::Duration;
using core::MonoTime;
using core::WallTime;

constexpr u32 kVersion = 7;
constexpr i64 kIssuedAt = 1'790'000'000'000'000;

[[nodiscard]] crypto::SigningKeyPair signing_key(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{fill});
    return crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
}

[[nodiscard]] crypto::KeyExchangeKeyPair exchange_key(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kKeyExchangeSecretKeySize> secret{};
    secret.fill(std::byte{fill});
    return crypto::key_exchange_key_pair_from_secret(crypto::KeyExchangeSecretKey(secret));
}

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text));
}

[[nodiscard]] std::string_view text_of(const std::span<const std::byte> bytes) {
    return core::as_chars(bytes);
}

// Mọi thứ server báo cho tầng trên trong một lần bơm gói.
struct ServerLog {
    std::vector<u32> connected;
    std::vector<u32> disconnected;
    std::vector<std::vector<std::byte>> payloads;
};

// Mọi thành viên công khai: thân TEST_F là lớp con, và fixture không có bất biến nào cần giấu.
class TransportTest : public ::testing::Test {
public:
    // Giới hạn tần suất mặc định, khoá hash cố định để bảng giới hạn theo địa chỉ tất định (X.4).
    [[nodiscard]] ServerConfig base_config() const {
        ServerConfig config{.identity = identity,
                            .token_signers = {auth.public_key},
                            .protocol_version = kVersion,
                            .max_connections = 4};
        crypto::ShortHashKey key;
        key.mutable_view()[0] = std::byte{0x77};
        config.address_hash_key = key;
        return config;
    }

    [[nodiscard]] ServerTransport make_server(ServerConfig config) const {
        Result<ServerTransport> created = ServerTransport::create(std::move(config), now);
        ORION_VERIFY(created.has_value(), "không tạo được server của test");
        return std::move(*created);
    }

    // REQUEST rồi RESPONSE bằng tay từ `from`; trả số gói server trả lời RESPONSE: 1 (ACCEPT hay
    // REJECT), hay 0 khi RESPONSE bị bỏ.
    [[nodiscard]] usize respond(const Address& from, const ConnectToken& token,
                                const u8 nonce_fill) {
        HandshakeNonce nonce;
        nonce.bytes.fill(std::byte{nonce_fill});
        server_out.clear();
        static_cast<void>(server.receive(
            from, write_request({.protocol_version = kVersion, .nonce = nonce, .token = token}),
            now, wall, server_out));
        if (server_out.packets().size() != 1) {
            ADD_FAILURE() << "REQUEST không được trả CHALLENGE";
            return 0;
        }
        const Cookie cookie =
            read_challenge(server_out.packets()[0].view()).value_or(Challenge{}).cookie;
        server_out.clear();
        static_cast<void>(server.receive(
            from,
            write_response(
                {.protocol_version = kVersion, .nonce = nonce, .cookie = cookie, .token = token}),
            now, wall, server_out));
        const usize replies = server_out.packets().size();
        server_out.clear();
        return replies;
    }

    [[nodiscard]] ConnectToken token_for(const crypto::KeyExchangeKeyPair& key,
                                         const u64 account = 42,
                                         const crypto::SigningKeyPair* signer = nullptr,
                                         const crypto::SigningPublicKey* server_key = nullptr,
                                         const i64 lifetime_seconds = 60) const {
        ConnectTokenClaims claims;
        claims.account_id = account;
        claims.issued_at = WallTime::from_unix_microseconds(kIssuedAt);
        claims.expires_at = claims.issued_at + Duration::seconds(lifetime_seconds);
        claims.client_key = key.public_key;
        claims.server_key = server_key != nullptr ? *server_key : identity.public_key;
        const Result<ConnectToken> token =
            issue_connect_token(claims, signer != nullptr ? *signer : auth);
        EXPECT_TRUE(token.has_value());
        return token.value_or(ConnectToken{});
    }

    // Kết quả của một lần bắt tay làm bằng tay (không qua ClientTransport).
    struct ManualHandshake {
        std::optional<SecureChannel> channel;
        std::optional<RejectReason> rejected;
    };

    // REQUEST, CHALLENGE, RESPONSE, rồi ACCEPT hay REJECT, bằng các hàm của handshake.hpp; khoá
    // phiên tính từ `key` và khoá X25519 trong ACCEPT. Kiểm được server mà không phụ thuộc client.
    [[nodiscard]] ManualHandshake manual_handshake(const ConnectToken& token,
                                                   const crypto::KeyExchangeKeyPair& key) {
        HandshakeNonce nonce;
        nonce.bytes.fill(std::byte{0x42});
        server_out.clear();
        static_cast<void>(server.receive(
            kClientAddress,
            write_request({.protocol_version = kVersion, .nonce = nonce, .token = token}), now,
            wall, server_out));
        EXPECT_EQ(server_out.packets().size(), 1U);
        const Result<Challenge> challenge = read_challenge(server_out.packets()[0].view());
        EXPECT_TRUE(challenge.has_value());
        server_out.clear();
        static_cast<void>(
            server.receive(kClientAddress,
                           write_response({.protocol_version = kVersion,
                                           .nonce = nonce,
                                           .cookie = challenge.value_or(Challenge{}).cookie,
                                           .token = token}),
                           now, wall, server_out));
        EXPECT_EQ(server_out.packets().size(), 1U);
        ManualHandshake result;
        const std::span<const std::byte> reply = server_out.packets()[0].view();
        if (const Result<Reject> reject = read_reject(reply); reject.has_value()) {
            result.rejected = reject->reason;
            return result;
        }
        const Result<Accept> accept = read_accept(reply);
        EXPECT_TRUE(accept.has_value());
        if (!accept) {
            return result;
        }
        Result<crypto::SessionKeys> keys =
            crypto::client_session_keys(key, accept->server_key_exchange);
        EXPECT_TRUE(keys.has_value());
        if (keys) {
            result.channel.emplace(accept->connection_id, std::move(*keys));
        }
        server_out.clear();
        return result;
    }

    [[nodiscard]] ClientTransport connect(const ConnectToken& token,
                                          const crypto::KeyExchangeKeyPair& key,
                                          const u32 version = kVersion) {
        Result<ClientTransport> client =
            ClientTransport::connect(kServerAddress, token, key, version, now, client_out);
        EXPECT_TRUE(client.has_value());
        return std::move(*client);
    }

    // Chuyển gói của client tới server (từ `from`) và gói của server tới client, tới khi không còn
    // gói nào, hay hết `rounds` vòng. `drop_to_client` bỏ gói thứ i server gửi (đếm từ 0).
    void pump(ClientTransport& client, const Address& from = kClientAddress, const u32 rounds = 8) {
        for (u32 round = 0; round < rounds; ++round) {
            if (client_out.empty() && server_out.empty()) {
                return;
            }
            const std::vector<OutgoingPacket> to_server(client_out.packets().begin(),
                                                        client_out.packets().end());
            client_out.clear();
            for (const OutgoingPacket& packet : to_server) {
                EXPECT_EQ(packet.to, kServerAddress);
                deliver_to_server(from, packet.view());
            }
            const std::vector<OutgoingPacket> to_client(server_out.packets().begin(),
                                                        server_out.packets().end());
            server_out.clear();
            for (const OutgoingPacket& packet : to_client) {
                const ClientEvent event =
                    client.receive(kServerAddress, packet.view(), now, client_out);
                if (!event.payload.empty()) {
                    client_payloads.emplace_back(event.payload.begin(), event.payload.end());
                }
            }
        }
    }

    void deliver_to_server(const Address& from, const std::span<const std::byte> packet) {
        const ServerEvent event = server.receive(from, packet, now, wall, server_out);
        if (event.connected) {
            events.connected.push_back(event.connection_id);
        }
        if (!event.payload.empty()) {
            events.payloads.emplace_back(event.payload.begin(), event.payload.end());
        }
        if (event.disconnected) {
            events.disconnected.push_back(event.connection_id);
        }
    }

    // Bắt tay trọn vẹn; trả client đã kết nối.
    [[nodiscard]] ClientTransport connected_client() {
        ClientTransport client = connect(token_for(client_key), client_key);
        pump(client);
        EXPECT_EQ(client.state(), ClientState::Connected);
        return client;
    }

    void advance(const Duration step) {
        now = now + step;
        wall = wall + step;
    }

    static constexpr Address kServerAddress = Address::v4({10, 0, 0, 1}, 7777);
    static constexpr Address kClientAddress = Address::v4({192, 0, 2, 10}, 50'000);

    crypto::SigningKeyPair auth = signing_key(0x11);
    crypto::SigningKeyPair identity = signing_key(0x33);
    crypto::KeyExchangeKeyPair client_key = exchange_key(0x55);
    MonoTime now = MonoTime::from_nanoseconds(1'000'000'000'000);
    WallTime wall = WallTime::from_unix_microseconds(kIssuedAt + 1'000'000);
    Outbox client_out{64};
    Outbox server_out{64};
    ServerTransport server = make_server(base_config());
    ServerLog events;
    std::vector<std::vector<std::byte>> client_payloads;
};

TEST_F(TransportTest, HandshakeThenDataBothWays) {
    ClientTransport client = connect(token_for(client_key), client_key);
    EXPECT_EQ(client.state(), ClientState::Connecting);
    pump(client);
    ASSERT_EQ(client.state(), ClientState::Connected);
    ASSERT_EQ(events.connected.size(), 1U);
    const u32 id = events.connected[0];
    EXPECT_EQ(client.connection_id(), id);
    EXPECT_EQ(server.connection_count(), 1U);
    EXPECT_EQ(server.address_of(id), std::optional<Address>{kClientAddress});

    ASSERT_TRUE(client.send(bytes_of("xin chào server"), now, client_out).has_value());
    pump(client);
    ASSERT_EQ(events.payloads.size(), 1U);
    EXPECT_EQ(text_of(events.payloads[0]), "xin chào server");

    ASSERT_TRUE(server.send(id, bytes_of("chào client"), now, server_out).has_value());
    pump(client);
    ASSERT_EQ(client_payloads.size(), 1U);
    EXPECT_EQ(text_of(client_payloads[0]), "chào client");
}

TEST_F(TransportTest, ConnectedEventCarriesTheAccount) {
    ClientTransport client = connect(token_for(client_key, 987'654), client_key);
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(kClientAddress, packet.view());
    }
    client_out.clear();
    const std::vector<OutgoingPacket> replies(server_out.packets().begin(),
                                              server_out.packets().end());
    server_out.clear();
    for (const OutgoingPacket& packet : replies) {
        static_cast<void>(client.receive(kServerAddress, packet.view(), now, client_out));
    }
    // RESPONSE lên server, ACCEPT về client, rồi keep-alive xác nhận lên server.
    for (u32 step = 0; step < 2; ++step) {
        const std::vector<OutgoingPacket> up(client_out.packets().begin(),
                                             client_out.packets().end());
        client_out.clear();
        for (const OutgoingPacket& packet : up) {
            const ServerEvent event =
                server.receive(kClientAddress, packet.view(), now, wall, server_out);
            if (event.connected) {
                EXPECT_EQ(event.account_id, 987'654U);
                events.connected.push_back(event.connection_id);
            }
        }
        const std::vector<OutgoingPacket> down(server_out.packets().begin(),
                                               server_out.packets().end());
        server_out.clear();
        for (const OutgoingPacket& packet : down) {
            static_cast<void>(client.receive(kServerAddress, packet.view(), now, client_out));
        }
    }
    EXPECT_EQ(events.connected.size(), 1U);
}

// Chống khuếch đại (X.9): mỗi gói server gửi khi nhận một gói chưa xác thực đều không lớn hơn gói
// đó; gói rác không được trả lời gì.
TEST_F(TransportTest, NoReplyIsLargerThanItsUnauthenticatedTrigger) {
    std::vector<std::vector<std::byte>> inputs;
    ClientTransport client = connect(token_for(client_key), client_key);
    inputs.emplace_back(client_out.packets()[0].view().begin(),
                        client_out.packets()[0].view().end());  // REQUEST thật
    std::vector<std::byte> wrong_version(inputs[0]);
    wrong_version[1] = std::byte{0x99};
    inputs.push_back(wrong_version);
    for (const usize size : {usize{1}, usize{18}, usize{49}, usize{117}, usize{249}, usize{1200}}) {
        for (const u8 type : {u8{1}, u8{3}, u8{6}, u8{9}}) {
            std::vector<std::byte> junk(size, std::byte{0x5A});
            junk[0] = std::byte{type};
            inputs.push_back(junk);
        }
    }
    client_out.clear();
    for (const std::vector<std::byte>& input : inputs) {
        server_out.clear();
        const ServerEvent event = server.receive(kClientAddress, input, now, wall, server_out);
        EXPECT_EQ(event.connection_id, 0U);
        for (const OutgoingPacket& reply : server_out.packets()) {
            EXPECT_LE(reply.size, input.size());
        }
        if (input.size() != kRequestSize && input.size() != kResponseSize) {
            EXPECT_TRUE(server_out.empty()) << "gói " << input.size() << " byte";
        }
    }
    // RESPONSE thật: ACCEPT (117 byte) nhỏ hơn RESPONSE (249 byte).
    server_out.clear();
    static_cast<void>(server.receive(kClientAddress, inputs[0], now, wall, server_out));
    ASSERT_EQ(server_out.packets().size(), 1U);
    static_cast<void>(
        client.receive(kServerAddress, server_out.packets()[0].view(), now, client_out));
    server_out.clear();
    ASSERT_EQ(client_out.packets().size(), 1U);
    const std::vector<std::byte> response(client_out.packets()[0].view().begin(),
                                          client_out.packets()[0].view().end());
    static_cast<void>(server.receive(kClientAddress, response, now, wall, server_out));
    ASSERT_EQ(server_out.packets().size(), 1U);
    EXPECT_EQ(server_out.packets()[0].size, kAcceptSize);
    EXPECT_LT(kAcceptSize, response.size());
}

TEST_F(TransportTest, VersionMismatchIsRejected) {
    ClientTransport client = connect(token_for(client_key), client_key, kVersion + 1);
    pump(client);
    EXPECT_EQ(client.state(), ClientState::Closed);
    EXPECT_EQ(client.close_reason(), CloseReason::Rejected);
    EXPECT_EQ(client.reject_reason(), std::optional{RejectReason::VersionMismatch});
    EXPECT_EQ(server.connection_count(), 0U);
}

TEST_F(TransportTest, BadTokensAreRejectedWithTheirReason) {
    const crypto::SigningKeyPair stranger = signing_key(0x22);
    const crypto::SigningPublicKey other_server = signing_key(0x44).public_key;
    struct Case {
        ConnectToken token;
        RejectReason reason;
    };
    const std::vector<Case> cases = {
        {token_for(client_key, 42, &stranger), RejectReason::TokenInvalid},
        {token_for(client_key, 42, nullptr, &other_server), RejectReason::TokenInvalid},
        {token_for(client_key, 42, nullptr, nullptr, 1), RejectReason::TokenExpired},
    };
    for (const Case& c : cases) {
        ClientTransport client = connect(c.token, client_key);
        pump(client);
        EXPECT_EQ(client.state(), ClientState::Closed);
        EXPECT_EQ(client.reject_reason(), std::optional{c.reason});
    }
    EXPECT_EQ(server.connection_count(), 0U);
}

TEST_F(TransportTest, ClientRefusesAKeyThatIsNotInTheToken) {
    const Result<ClientTransport> client = ClientTransport::connect(
        kServerAddress, token_for(client_key), exchange_key(0x66), kVersion, now, client_out);
    ASSERT_FALSE(client.has_value());
    EXPECT_EQ(client.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(client_out.empty());
}

// Một token lập được đúng một kết nối, kể cả sau khi kết nối đó đóng.
TEST_F(TransportTest, TokenOpensOneConnectionOnly) {
    const ConnectToken token = token_for(client_key);
    ClientTransport first = connect(token, client_key);
    pump(first);
    ASSERT_EQ(first.state(), ClientState::Connected);
    ClientTransport second = connect(token, client_key);
    pump(second);
    EXPECT_EQ(second.reject_reason(), std::optional{RejectReason::TokenUsed});
    first.disconnect(client_out);
    pump(first);
    EXPECT_EQ(events.disconnected.size(), 1U);
    ClientTransport third = connect(token, client_key);
    pump(third);
    EXPECT_EQ(third.reject_reason(), std::optional{RejectReason::TokenUsed});
}

TEST_F(TransportTest, ServerFullIsRejected) {
    std::vector<ClientTransport> clients;
    for (u8 i = 0; i < 4; ++i) {
        const crypto::KeyExchangeKeyPair key = exchange_key(static_cast<u8>(0x70 + i));
        clients.push_back(connect(token_for(key), key));
        pump(clients.back());
        ASSERT_EQ(clients.back().state(), ClientState::Connected);
    }
    const crypto::KeyExchangeKeyPair key = exchange_key(0x7F);
    ClientTransport extra = connect(token_for(key), key);
    pump(extra);
    EXPECT_EQ(extra.reject_reason(), std::optional{RejectReason::ServerFull});
}

// Mất REQUEST, mất CHALLENGE, mất ACCEPT: client gửi lại sau kHandshakeResendInterval và vẫn kết
// nối được; ACCEPT gửi lại là đúng gói cũ, nên chỉ một kết nối được tạo.
TEST_F(TransportTest, LostHandshakePacketsAreResent) {
    ClientTransport client = connect(token_for(client_key), client_key);
    client_out.clear();  // REQUEST mất
    advance(kHandshakeResendInterval);
    static_cast<void>(client.update(now, client_out));
    ASSERT_EQ(client_out.packets().size(), 1U);
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(kClientAddress, packet.view());
    }
    client_out.clear();
    server_out.clear();  // CHALLENGE mất
    advance(kHandshakeResendInterval);
    static_cast<void>(client.update(now, client_out));
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(kClientAddress, packet.view());
    }
    client_out.clear();
    ASSERT_EQ(server_out.packets().size(), 1U);
    static_cast<void>(
        client.receive(kServerAddress, server_out.packets()[0].view(), now, client_out));
    server_out.clear();
    // RESPONSE tới server, ACCEPT mất.
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(kClientAddress, packet.view());
    }
    client_out.clear();
    ASSERT_EQ(server_out.packets().size(), 1U);
    const std::vector<std::byte> first_accept(server_out.packets()[0].view().begin(),
                                              server_out.packets()[0].view().end());
    server_out.clear();
    advance(kHandshakeResendInterval);
    static_cast<void>(client.update(now, client_out));
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(kClientAddress, packet.view());
    }
    client_out.clear();
    ASSERT_EQ(server_out.packets().size(), 1U);
    EXPECT_TRUE(std::ranges::equal(server_out.packets()[0].view(), first_accept));
    pump(client);
    EXPECT_EQ(client.state(), ClientState::Connected);
    EXPECT_EQ(server.connection_count(), 1U);
    EXPECT_EQ(events.connected.size(), 1U);
}

TEST_F(TransportTest, HandshakeGivesUpAfterTheTimeout) {
    ClientTransport client = connect(token_for(client_key), client_key);
    client_out.clear();
    ClientEvent event;
    while (!event.closed) {
        advance(kHandshakeResendInterval);
        event = client.update(now, client_out);
        client_out.clear();  // Mạng mất hết.
    }
    EXPECT_EQ(client.close_reason(), CloseReason::HandshakeTimeout);
    EXPECT_FALSE(client.send(bytes_of("x"), now, client_out).has_value());
}

// Cookie gắn với địa chỉ nguồn: RESPONSE từ địa chỉ khác bị bỏ, không trả lời.
TEST_F(TransportTest, ResponseFromAnotherAddressGetsNothing) {
    ClientTransport client = connect(token_for(client_key), client_key);
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(kClientAddress, packet.view());
    }
    client_out.clear();
    static_cast<void>(
        client.receive(kServerAddress, server_out.packets()[0].view(), now, client_out));
    server_out.clear();
    const Address spoofed = Address::v4({198, 51, 100, 9}, 50'000);
    for (const OutgoingPacket& packet : client_out.packets()) {
        deliver_to_server(spoofed, packet.view());
    }
    EXPECT_TRUE(server_out.empty());
    EXPECT_EQ(server.connection_count(), 0U);
}

// Kẻ đứng giữa thay khoá X25519 của server trong ACCEPT: chữ ký không còn khớp, client bỏ gói.
TEST_F(TransportTest, TamperedAcceptIsIgnored) {
    ClientTransport client = connect(token_for(client_key), client_key);
    for (u32 step = 0; step < 2; ++step) {
        const std::vector<OutgoingPacket> up(client_out.packets().begin(),
                                             client_out.packets().end());
        client_out.clear();
        for (const OutgoingPacket& packet : up) {
            deliver_to_server(kClientAddress, packet.view());
        }
        std::vector<OutgoingPacket> down(server_out.packets().begin(), server_out.packets().end());
        server_out.clear();
        for (OutgoingPacket& packet : down) {
            if (packet.size == kAcceptSize) {
                packet.bytes[30] ^= std::byte{0x01};  // trong server_key_exchange
            }
            static_cast<void>(client.receive(kServerAddress, packet.view(), now, client_out));
        }
    }
    EXPECT_EQ(client.state(), ClientState::Connecting);
}

TEST_F(TransportTest, ReplayedDataIsDropped) {
    ClientTransport client = connected_client();
    ASSERT_TRUE(client.send(bytes_of("một lần"), now, client_out).has_value());
    const std::vector<std::byte> packet(client_out.packets()[0].view().begin(),
                                        client_out.packets()[0].view().end());
    pump(client);
    ASSERT_EQ(events.payloads.size(), 1U);
    deliver_to_server(kClientAddress, packet);
    deliver_to_server(Address::v4({198, 51, 100, 9}, 1), packet);
    EXPECT_EQ(events.payloads.size(), 1U);
}

// Client đổi mạng: gói mới nhất từ địa chỉ mới chuyển kết nối sang đó; gói cũ hơn từ nơi khác thì
// vẫn được nhận nhưng không chuyển địa chỉ.
TEST_F(TransportTest, NewestAuthenticPacketMovesTheConnection) {
    ClientTransport client = connected_client();
    const u32 id = client.connection_id();
    ASSERT_TRUE(client.send(bytes_of("gói 1"), now, client_out).has_value());
    ASSERT_TRUE(client.send(bytes_of("gói 2"), now, client_out).has_value());
    const std::vector<std::byte> older(client_out.packets()[0].view().begin(),
                                       client_out.packets()[0].view().end());
    const std::vector<std::byte> newer(client_out.packets()[1].view().begin(),
                                       client_out.packets()[1].view().end());
    client_out.clear();
    const Address roamed = Address::v4({203, 0, 113, 50}, 61'000);
    deliver_to_server(roamed, newer);
    EXPECT_EQ(server.address_of(id), std::optional<Address>{roamed});
    deliver_to_server(kClientAddress, older);
    EXPECT_EQ(server.address_of(id), std::optional<Address>{roamed});
    EXPECT_EQ(events.payloads.size(), 2U);
}

TEST_F(TransportTest, KeepAlivesHoldAnIdleConnection) {
    ClientTransport client = connected_client();
    std::array<u32, 4> closed{};
    for (u32 second = 0; second < 60; ++second) {
        advance(Duration::seconds(1));
        EXPECT_FALSE(client.update(now, client_out).closed);
        EXPECT_EQ(server.update(now, wall, server_out, closed), 0U);
        pump(client);
    }
    EXPECT_EQ(client.state(), ClientState::Connected);
    EXPECT_EQ(server.connection_count(), 1U);
}

TEST_F(TransportTest, SilentPeersTimeOut) {
    ClientTransport client = connected_client();
    const u32 id = client.connection_id();
    std::array<u32, 4> closed{};
    advance(kIdleTimeout);
    ASSERT_EQ(server.update(now, wall, server_out, closed), 1U);
    EXPECT_EQ(closed[0], id);
    EXPECT_EQ(server.connection_count(), 0U);
    EXPECT_TRUE(client.update(now, client_out).closed);
    EXPECT_EQ(client.close_reason(), CloseReason::Timeout);
}

// Kết nối chờ không được xác nhận thì bị bỏ sau kPendingTimeout, không báo lên tầng trên.
TEST_F(TransportTest, UnconfirmedConnectionExpiresQuietly) {
    ClientTransport client = connect(token_for(client_key), client_key);
    for (u32 step = 0; step < 2; ++step) {
        const std::vector<OutgoingPacket> up(client_out.packets().begin(),
                                             client_out.packets().end());
        client_out.clear();
        for (const OutgoingPacket& packet : up) {
            deliver_to_server(kClientAddress, packet.view());
        }
        const std::vector<OutgoingPacket> down(server_out.packets().begin(),
                                               server_out.packets().end());
        server_out.clear();
        for (const OutgoingPacket& packet : down) {
            static_cast<void>(client.receive(kServerAddress, packet.view(), now, client_out));
        }
    }
    client_out.clear();  // Keep-alive xác nhận bị mất.
    ASSERT_EQ(server.connection_count(), 1U);
    std::array<u32, 4> closed{};
    advance(kPendingTimeout);
    EXPECT_EQ(server.update(now, wall, server_out, closed), 0U);
    EXPECT_EQ(server.connection_count(), 0U);
    EXPECT_TRUE(events.connected.empty());
}

TEST_F(TransportTest, EitherSideCanDisconnect) {
    ClientTransport client = connected_client();
    server.disconnect(client.connection_id(), server_out);
    EXPECT_EQ(server.connection_count(), 0U);
    pump(client);
    EXPECT_EQ(client.state(), ClientState::Closed);
    EXPECT_EQ(client.close_reason(), CloseReason::ServerDisconnected);

    const crypto::KeyExchangeKeyPair key = exchange_key(0x56);
    ClientTransport other = connect(token_for(key), key);
    pump(other);
    ASSERT_EQ(other.state(), ClientState::Connected);
    other.disconnect(client_out);
    EXPECT_EQ(client_out.packets().size(), kDisconnectRepeats);
    pump(other);
    EXPECT_EQ(other.close_reason(), CloseReason::LocalDisconnect);
    EXPECT_EQ(events.disconnected.size(), 1U);
    EXPECT_EQ(server.connection_count(), 0U);
}

TEST_F(TransportTest, SendChecksStateAndSize) {
    EXPECT_EQ(server.send(12'345, bytes_of("x"), now, server_out).error().code(),
              ErrorCode::NotFound);
    ClientTransport client = connected_client();
    const std::vector<std::byte> largest(kMaxTransportPayload, std::byte{7});
    EXPECT_TRUE(client.send(largest, now, client_out).has_value());
    const std::vector<std::byte> too_large(kMaxTransportPayload + 1);
    EXPECT_EQ(client.send(too_large, now, client_out).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(client.send({}, now, client_out).error().code(), ErrorCode::InvalidArgument);
    pump(client);
    ASSERT_EQ(events.payloads.size(), 1U);
    EXPECT_EQ(events.payloads[0].size(), kMaxTransportPayload);
}

// Gói không từ địa chỉ server, và REJECT hay CHALLENGE với nonce khác, bị client bỏ qua.
TEST_F(TransportTest, ClientIgnoresForeignPackets) {
    ClientTransport client = connect(token_for(client_key), client_key);
    const std::array<std::byte, kRejectSize> reject =
        write_reject({.nonce = {}, .reason = RejectReason::ServerFull});
    static_cast<void>(client.receive(kServerAddress, reject, now, client_out));
    static_cast<void>(client.receive(Address::v4({1, 2, 3, 4}, 5), reject, now, client_out));
    EXPECT_EQ(client.state(), ClientState::Connecting);
    pump(client);
    EXPECT_EQ(client.state(), ClientState::Connected);
}

// RESPONSE có cookie đúng tốn một lượt của địa chỉ nguồn (IPv4 theo cả địa chỉ, không theo cổng).
// Hết lượt thì server không trả lời gì, kể cả REJECT; địa chỉ khác vẫn được; lượt hồi theo thời
// gian.
TEST_F(TransportTest, ResponsesAreLimitedPerSourceAddress) {
    ServerConfig config = base_config();
    config.response_limit_per_address = RateLimit::per_second(1, 2);
    server = make_server(std::move(config));
    const Address first = Address::v4({192, 0, 2, 10}, 50'001);
    const Address second = Address::v4({192, 0, 2, 10}, 50'002);
    const Address other = Address::v4({198, 51, 100, 7}, 50'000);
    EXPECT_EQ(respond(first, token_for(exchange_key(0x61)), 1), 1U);
    EXPECT_EQ(respond(second, token_for(exchange_key(0x62)), 2), 1U);
    EXPECT_EQ(respond(first, token_for(exchange_key(0x63)), 3), 0U);
    EXPECT_EQ(server.connection_count(), 2U);
    EXPECT_EQ(respond(other, token_for(exchange_key(0x63)), 4), 1U);
    advance(Duration::seconds(1));
    EXPECT_EQ(respond(second, token_for(exchange_key(0x64)), 5), 1U);
    EXPECT_EQ(server.connection_count(), 4U);
}

// Giới hạn chung của RESPONSE: đủ nhiều địa chỉ khác nhau cũng không vượt được.
TEST_F(TransportTest, ResponsesAreLimitedForTheWholeServer) {
    ServerConfig config = base_config();
    config.response_limit = RateLimit::per_second(1, 2);
    server = make_server(std::move(config));
    EXPECT_EQ(respond(Address::v4({192, 0, 2, 1}, 1), token_for(exchange_key(0x61)), 1), 1U);
    EXPECT_EQ(respond(Address::v4({192, 0, 2, 2}, 1), token_for(exchange_key(0x62)), 2), 1U);
    EXPECT_EQ(respond(Address::v4({192, 0, 2, 3}, 1), token_for(exchange_key(0x63)), 3), 0U);
    advance(Duration::seconds(1));
    EXPECT_EQ(respond(Address::v4({192, 0, 2, 3}, 1), token_for(exchange_key(0x63)), 4), 1U);
}

// REQUEST, kể cả REQUEST lệch version (vốn được trả REJECT), tốn lượt chung của cả server.
TEST_F(TransportTest, RequestsAreLimitedForTheWholeServer) {
    ServerConfig config = base_config();
    config.request_limit = RateLimit::per_second(1, 2);
    server = make_server(std::move(config));
    const ConnectToken token = token_for(client_key);
    const auto request = [&](const u32 version) {
        server_out.clear();
        static_cast<void>(server.receive(
            kClientAddress,
            write_request({.protocol_version = version, .nonce = {}, .token = token}), now, wall,
            server_out));
        return server_out.packets().size();
    };
    EXPECT_EQ(request(kVersion), 1U);
    EXPECT_EQ(request(kVersion + 1), 1U);  // REJECT lý do 1.
    EXPECT_EQ(request(kVersion), 0U);
    EXPECT_EQ(request(kVersion + 1), 0U);
    advance(Duration::seconds(1));
    EXPECT_EQ(request(kVersion), 1U);
    server_out.clear();
}

// Gói dữ liệu đã xác thực tốn lượt của kết nối; gói vượt bị bỏ mà kết nối vẫn còn.
TEST_F(TransportTest, AuthenticatedPacketsAreLimitedPerConnection) {
    ServerConfig config = base_config();
    config.packet_limit_per_connection = RateLimit::per_second(10, 3);
    server = make_server(std::move(config));
    ClientTransport client = connected_client();  // Keep-alive xác nhận kết nối tốn một lượt.
    for (u32 i = 0; i < 5; ++i) {
        ASSERT_TRUE(client.send(bytes_of("x"), now, client_out).has_value());
    }
    pump(client);
    EXPECT_EQ(events.payloads.size(), 2U);
    advance(Duration::milliseconds(100));
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.send(bytes_of("y"), now, client_out).has_value());
    }
    pump(client);
    ASSERT_EQ(events.payloads.size(), 3U);
    EXPECT_EQ(text_of(events.payloads[2]), "y");
    EXPECT_EQ(server.connection_count(), 1U);
}

TEST(ServerTransportConfig, CreateRejectsAnEmptyConfiguration) {
    const crypto::SigningKeyPair identity = signing_key(0x33);
    const MonoTime now = MonoTime::from_nanoseconds(0);
    EXPECT_FALSE(ServerTransport::create({.identity = identity,
                                          .token_signers = {identity.public_key},
                                          .protocol_version = kVersion,
                                          .max_connections = 0},
                                         now)
                     .has_value());
    EXPECT_FALSE(ServerTransport::create({.identity = identity,
                                          .token_signers = {},
                                          .protocol_version = kVersion,
                                          .max_connections = 1},
                                         now)
                     .has_value());
}

TEST(ServerTransportConfig, CreateRejectsInvalidRateLimits) {
    const crypto::SigningKeyPair identity = signing_key(0x33);
    const MonoTime now = MonoTime::from_nanoseconds(0);
    const ServerConfig valid{.identity = identity,
                             .token_signers = {identity.public_key},
                             .protocol_version = kVersion,
                             .max_connections = 1};
    EXPECT_TRUE(ServerTransport::create(valid, now).has_value());
    const RateLimit broken = RateLimit::per_second(10, 0);
    ServerConfig config = valid;
    config.request_limit = broken;
    EXPECT_FALSE(ServerTransport::create(config, now).has_value());
    config = valid;
    config.response_limit_per_address = broken;
    EXPECT_FALSE(ServerTransport::create(config, now).has_value());
    config = valid;
    config.response_limit = broken;
    EXPECT_FALSE(ServerTransport::create(config, now).has_value());
    config = valid;
    config.packet_limit_per_connection = broken;
    EXPECT_FALSE(ServerTransport::create(config, now).has_value());
    config = valid;
    config.address_slots = 0;
    EXPECT_FALSE(ServerTransport::create(config, now).has_value());
    config.address_slots = kMaxAddressLimiterSlots + 1;
    EXPECT_FALSE(ServerTransport::create(config, now).has_value());
    config.address_slots = kMaxAddressLimiterSlots;
    EXPECT_TRUE(ServerTransport::create(config, now).has_value());
}

TEST_F(TransportTest, ManualHandshakeGetsAWorkingChannel) {
    ManualHandshake handshake = manual_handshake(token_for(client_key), client_key);
    if (!handshake.channel.has_value()) {
        FAIL() << "bắt tay bằng tay không ra kênh";
    }
    std::array<std::byte, kMaxPacketSize> packet{};
    const Result<usize> size =
        seal_payload(*handshake.channel, PayloadKind::Data, bytes_of("bằng tay"), packet);
    ASSERT_TRUE(size.has_value());
    deliver_to_server(kClientAddress, std::span(packet).first(*size));
    ASSERT_EQ(events.connected.size(), 1U);
    ASSERT_EQ(events.payloads.size(), 1U);
    EXPECT_EQ(text_of(events.payloads[0]), "bằng tay");
}

// Khoá X25519 toàn 0 của client (điểm bậc thấp) được auth ký vào token, nhưng không trao được khoá.
TEST_F(TransportTest, LowOrderClientKeyIsRejected) {
    crypto::KeyExchangeKeyPair low_order = client_key;
    low_order.public_key = {};
    const ManualHandshake handshake = manual_handshake(token_for(low_order), low_order);
    EXPECT_FALSE(handshake.channel.has_value());
    EXPECT_EQ(handshake.rejected, std::optional{RejectReason::TokenInvalid});
    EXPECT_EQ(server.connection_count(), 0U);
}

// Gói đúng khoá mà payload sai luật: nội dung bị bỏ, kết nối vẫn được xác nhận và giữ.
TEST_F(TransportTest, MalformedAuthenticatedPayloadIsIgnored) {
    ManualHandshake handshake = manual_handshake(token_for(client_key), client_key);
    if (!handshake.channel.has_value()) {
        FAIL() << "bắt tay bằng tay không ra kênh";
    }
    std::array<std::byte, kMaxPacketSize> packet{};
    const std::array unknown_kind = {std::byte{9}, std::byte{1}};
    const Result<usize> size = handshake.channel->seal(packet, unknown_kind);
    ASSERT_TRUE(size.has_value());
    const ServerEvent event =
        server.receive(kClientAddress, std::span(packet).first(*size), now, wall, server_out);
    EXPECT_TRUE(event.connected);
    EXPECT_TRUE(event.payload.empty());
    EXPECT_FALSE(event.disconnected);
    EXPECT_EQ(server.connection_count(), 1U);
}

// ACCEPT được ký đúng nhưng khoá X25519 của server là điểm bậc thấp: client đóng với lý do riêng.
TEST_F(TransportTest, LowOrderServerKeyClosesTheClient) {
    ClientTransport client = connect(token_for(client_key), client_key);
    const std::vector<OutgoingPacket> request(client_out.packets().begin(),
                                              client_out.packets().end());
    client_out.clear();
    for (const OutgoingPacket& packet : request) {
        deliver_to_server(kClientAddress, packet.view());
    }
    static_cast<void>(
        client.receive(kServerAddress, server_out.packets()[0].view(), now, client_out));
    server_out.clear();
    const Result<Response> response = read_response(client_out.packets()[0].view());
    ASSERT_TRUE(response.has_value());
    Accept accept{
        .nonce = response->nonce, .connection_id = 1, .server_key_exchange = {}, .signature = {}};
    accept.signature =
        sign_accept(kVersion, accept.nonce, token_hash(response->token), accept.connection_id,
                    accept.server_key_exchange, identity.secret_key);
    const ClientEvent event = client.receive(kServerAddress, write_accept(accept), now, client_out);
    EXPECT_TRUE(event.closed);
    EXPECT_EQ(client.close_reason(), CloseReason::KeyExchangeFailed);
}

TEST_F(TransportTest, ClientRejectsAMalformedToken) {
    const Result<ClientTransport> client = ClientTransport::connect(
        kServerAddress, ConnectToken{}, client_key, kVersion, now, client_out);
    ASSERT_FALSE(client.has_value());
    EXPECT_EQ(client.error().code(), ErrorCode::InvalidArgument);
}

// Gói dữ liệu tới client đang bắt tay bị bỏ qua.
TEST_F(TransportTest, DataBeforeTheHandshakeEndsIsIgnored) {
    const ClientTransport connected = connected_client();
    ASSERT_TRUE(server.send(connected.connection_id(), bytes_of("x"), now, server_out).has_value());
    const std::vector<std::byte> data(server_out.packets()[0].view().begin(),
                                      server_out.packets()[0].view().end());
    server_out.clear();
    const crypto::KeyExchangeKeyPair key = exchange_key(0x57);
    ClientTransport connecting = connect(token_for(key), key);
    const ClientEvent event = connecting.receive(kServerAddress, data, now, client_out);
    EXPECT_TRUE(event.payload.empty());
    EXPECT_EQ(connecting.state(), ClientState::Connecting);
}

// `closed` chỉ có một chỗ: kết nối hết hạn thứ hai được báo ở lần update sau.
TEST_F(TransportTest, ClosedConnectionsBeyondTheSpanAreReportedLater) {
    const ClientTransport first = connected_client();
    const crypto::KeyExchangeKeyPair key = exchange_key(0x58);
    ClientTransport second = connect(token_for(key), key);
    pump(second);
    ASSERT_EQ(server.connection_count(), 2U);
    advance(kIdleTimeout);
    std::array<u32, 1> closed{};
    EXPECT_EQ(server.update(now, wall, server_out, closed), 1U);
    EXPECT_EQ(server.connection_count(), 1U);
    EXPECT_EQ(server.update(now, wall, server_out, closed), 1U);
    EXPECT_EQ(server.connection_count(), 0U);
    EXPECT_EQ(server.update(now, wall, server_out, closed), 0U);
}

// Bảng token đã dùng giữ 4 lần sức chứa: đầy thì kết nối mới bị từ chối, kể cả khi không còn kết
// nối nào, cho tới khi token cũ hết hạn và được quên.
TEST_F(TransportTest, TokenTableFullUntilTokensExpire) {
    for (u8 i = 0; i < 16; ++i) {
        const crypto::KeyExchangeKeyPair key = exchange_key(static_cast<u8>(0x80 + i));
        ClientTransport client = connect(token_for(key, 42, nullptr, nullptr, 30), key);
        pump(client);
        ASSERT_EQ(client.state(), ClientState::Connected) << int{i};
        client.disconnect(client_out);
        pump(client);
    }
    ASSERT_EQ(server.connection_count(), 0U);
    const crypto::KeyExchangeKeyPair key = exchange_key(0x9F);
    ClientTransport refused = connect(token_for(key, 42, nullptr, nullptr, 60), key);
    pump(refused);
    EXPECT_EQ(refused.reject_reason(), std::optional{RejectReason::ServerFull});
    // 30 giây sau lúc ký, 16 token đầu hết hạn; update quên chúng.
    advance(Duration::seconds(30));
    std::array<u32, 4> closed{};
    static_cast<void>(server.update(now, wall, server_out, closed));
    const crypto::KeyExchangeKeyPair late = exchange_key(0xA0);
    ClientTransport accepted = connect(token_for(late, 42, nullptr, nullptr, 60), late);
    pump(accepted);
    EXPECT_EQ(accepted.state(), ClientState::Connected);
}

TEST_F(TransportTest, TransportsCanBeMoved) {
    ClientTransport client = connected_client();
    const u32 id = client.connection_id();
    ClientTransport moved = std::move(client);
    EXPECT_EQ(moved.connection_id(), id);
    const crypto::KeyExchangeKeyPair key = exchange_key(0x59);
    ClientTransport other = connect(token_for(key), key);
    moved = std::move(other);
    EXPECT_EQ(moved.state(), ClientState::Connecting);
    ServerTransport replacement = make_server(base_config());
    replacement = std::move(server);
    EXPECT_EQ(replacement.connection_count(), 1U);
}

// X.7: sau bắt tay, gửi và nhận gói dữ liệu ở cả hai phía không cấp phát.
TEST_F(TransportTest, DataPathDoesNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    ClientTransport client = connected_client();
    const u32 id = client.connection_id();
    std::array<u32, 4> closed{};
    std::array<OutgoingPacket, 2> wire{};
    const core::testing::AllocationScope scope;
    for (u32 i = 0; i < 100; ++i) {
        ASSERT_TRUE(client.send(bytes_of("lên"), now, client_out).has_value());
        wire[0] = client_out.packets()[0];
        client_out.clear();
        const ServerEvent up =
            server.receive(kClientAddress, wire[0].view(), now, wall, server_out);
        ASSERT_EQ(up.payload.size(), 4U);
        ASSERT_TRUE(server.send(id, bytes_of("xuống"), now, server_out).has_value());
        wire[1] = server_out.packets()[0];
        server_out.clear();
        const ClientEvent down = client.receive(kServerAddress, wire[1].view(), now, client_out);
        ASSERT_FALSE(down.payload.empty());
        advance(Duration::milliseconds(20));
        static_cast<void>(client.update(now, client_out));
        static_cast<void>(server.update(now, wall, server_out, closed));
        client_out.clear();
        server_out.clear();
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::net
