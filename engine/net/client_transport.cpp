#include "engine/net/client_transport.hpp"

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/connection.hpp"
#include "engine/net/handshake.hpp"
#include "engine/net/outbox.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/secure_channel.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace orion::net {
namespace {

enum class Phase : u8 {
    SendingRequest,
    SendingResponse,
    Connected,
    Closed,
};

}  // namespace

struct ClientTransport::State {
    State(const Address& server_address, const ConnectToken& connect_token,
          crypto::KeyExchangeKeyPair key_pair, const crypto::SigningPublicKey& identity,
          const u32 version, const core::MonoTime now) noexcept
        : server(server_address),
          token(connect_token),
          hash(token_hash(connect_token)),
          key(std::move(key_pair)),
          server_identity(identity),
          protocol_version(version),
          started_at(now),
          last_handshake_send(now) {
        crypto::random_bytes(nonce.bytes);
    }

    void send_handshake(const core::MonoTime now, Outbox& outbox) noexcept {
        if (phase == Phase::SendingRequest) {
            static_cast<void>(outbox.push(
                server,
                write_request(
                    {.protocol_version = protocol_version, .nonce = nonce, .token = token})));
        } else {
            static_cast<void>(
                outbox.push(server, write_response({.protocol_version = protocol_version,
                                                    .nonce = nonce,
                                                    .cookie = cookie,
                                                    .token = token})));
        }
        last_handshake_send = now;
    }

    // Đóng kết nối; trả sự kiện cho bên gọi.
    [[nodiscard]] ClientEvent close(const CloseReason reason) noexcept {
        phase = Phase::Closed;
        close_reason = reason;
        channel.reset();
        return {.connected = false, .closed = true, .payload = {}};
    }

    [[nodiscard]] Result<void> transmit(const PayloadKind kind,
                                        const std::span<const std::byte> data,
                                        const core::MonoTime now, Outbox& outbox) noexcept {
        if (!channel.has_value()) {
            return fail(ErrorCode::FailedPrecondition, "net: client chưa có khoá phiên");
        }
        const Result<usize> sealed = seal_payload(*channel, kind, data, packet);
        if (!sealed) {
            return std::unexpected(sealed.error());
        }
        static_cast<void>(outbox.push(server, std::span(packet).first(*sealed)));
        last_sent = now;
        return {};
    }

    [[nodiscard]] ClientEvent on_challenge(std::span<const std::byte> bytes, core::MonoTime now,
                                           Outbox& outbox) noexcept;
    [[nodiscard]] ClientEvent on_accept(std::span<const std::byte> bytes, core::MonoTime now,
                                        Outbox& outbox) noexcept;
    [[nodiscard]] ClientEvent on_reject(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] ClientEvent on_data(std::span<const std::byte> bytes,
                                      core::MonoTime now) noexcept;

    Address server;
    ConnectToken token;
    crypto::Hash hash;
    crypto::KeyExchangeKeyPair key;
    crypto::SigningPublicKey server_identity;
    u32 protocol_version;
    HandshakeNonce nonce;
    Cookie cookie;
    Phase phase = Phase::SendingRequest;
    CloseReason close_reason = CloseReason::None;
    std::optional<RejectReason> reject_reason;
    std::optional<SecureChannel> channel;
    core::MonoTime started_at;
    core::MonoTime last_handshake_send;
    core::MonoTime last_received;
    core::MonoTime last_sent;
    std::array<std::byte, kMaxPacketSize> plaintext{};
    std::array<std::byte, kMaxPacketSize> packet{};
};

ClientEvent ClientTransport::State::on_challenge(const std::span<const std::byte> bytes,
                                                 const core::MonoTime now,
                                                 Outbox& outbox) noexcept {
    if (phase != Phase::SendingRequest && phase != Phase::SendingResponse) {
        return {};
    }
    const Result<Challenge> challenge = read_challenge(bytes);
    if (!challenge || !crypto::equal_constant_time(challenge->nonce, nonce)) {
        return {};
    }
    // CHALLENGE lặp lại (REQUEST được gửi lại) mang cookie mới hơn: dùng cookie đó.
    cookie = challenge->cookie;
    phase = Phase::SendingResponse;
    send_handshake(now, outbox);
    return {};
}

ClientEvent ClientTransport::State::on_accept(const std::span<const std::byte> bytes,
                                              const core::MonoTime now, Outbox& outbox) noexcept {
    if (phase != Phase::SendingResponse) {
        return {};
    }
    const Result<Accept> accept = read_accept(bytes);
    if (!accept || !crypto::equal_constant_time(accept->nonce, nonce) ||
        !verify_accept(*accept, protocol_version, hash, server_identity)) {
        return {};  // Không phải ACCEPT của server cho lần thử này: tiếp tục chờ.
    }
    Result<crypto::SessionKeys> keys =
        crypto::client_session_keys(key, accept->server_key_exchange);
    if (!keys) {
        return close(CloseReason::KeyExchangeFailed);
    }
    channel.emplace(accept->connection_id, std::move(*keys));
    phase = Phase::Connected;
    last_received = now;
    // Gói đầu tiên xác nhận kết nối với server.
    if (!transmit(PayloadKind::KeepAlive, {}, now, outbox)) {
        return close(CloseReason::SequenceExhausted);
    }
    return {.connected = true, .closed = false, .payload = {}};
}

ClientEvent ClientTransport::State::on_reject(const std::span<const std::byte> bytes) noexcept {
    if (phase != Phase::SendingRequest && phase != Phase::SendingResponse) {
        return {};
    }
    const Result<Reject> reject = read_reject(bytes);
    if (!reject || !crypto::equal_constant_time(reject->nonce, nonce)) {
        return {};
    }
    reject_reason = reject->reason;
    return close(CloseReason::Rejected);
}

ClientEvent ClientTransport::State::on_data(const std::span<const std::byte> bytes,
                                            const core::MonoTime now) noexcept {
    if (phase != Phase::Connected || !channel.has_value()) {
        return {};
    }
    const Result<usize> opened = channel->open(plaintext, bytes);
    if (!opened) {
        return {};
    }
    last_received = now;
    const std::optional<Payload> payload = parse_payload(std::span(plaintext).first(*opened));
    if (!payload.has_value()) {
        return {};
    }
    if (payload->kind == PayloadKind::Disconnect) {
        return close(CloseReason::ServerDisconnected);
    }
    return {.connected = false, .closed = false, .payload = payload->data};
}

ClientTransport::ClientTransport(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
ClientTransport::ClientTransport(ClientTransport&&) noexcept = default;
ClientTransport& ClientTransport::operator=(ClientTransport&&) noexcept = default;
ClientTransport::~ClientTransport() = default;

Result<ClientTransport> ClientTransport::connect(const Address& server, const ConnectToken& token,
                                                 const crypto::KeyExchangeKeyPair& key,
                                                 const u32 protocol_version,
                                                 const core::MonoTime now, Outbox& outbox) {
    const Result<ConnectTokenClaims> claims = inspect_connect_token(token.view());
    if (!claims) {
        return std::unexpected(claims.error());
    }
    if (!crypto::equal_constant_time(claims->client_key, key.public_key)) {
        return fail(ErrorCode::InvalidArgument, "net: khoá X25519 khác client_key của token");
    }
    auto state =
        std::make_unique<State>(server, token, key, claims->server_key, protocol_version, now);
    state->send_handshake(now, outbox);
    return ClientTransport(std::move(state));
}

ClientEvent ClientTransport::receive(const Address& from, const std::span<const std::byte> packet,
                                     const core::MonoTime now, Outbox& outbox) noexcept {
    if (from != state_->server) {
        return {};
    }
    const Result<PacketType> type = packet_type(packet);
    if (!type) {
        return {};
    }
    switch (*type) {
        case PacketType::Challenge:
            return state_->on_challenge(packet, now, outbox);
        case PacketType::Accept:
            return state_->on_accept(packet, now, outbox);
        case PacketType::Reject:
            return state_->on_reject(packet);
        case PacketType::Data:
            return state_->on_data(packet, now);
        case PacketType::Request:
        case PacketType::Response:
            return {};  // Chỉ client gửi các gói này.
    }
    return {};
}

Result<void> ClientTransport::send(const std::span<const std::byte> payload,
                                   const core::MonoTime now, Outbox& outbox) noexcept {
    if (state_->phase != Phase::Connected) {
        return fail(ErrorCode::FailedPrecondition, "net: client chưa kết nối");
    }
    const Result<void> sent = state_->transmit(PayloadKind::Data, payload, now, outbox);
    if (!sent && sent.error().code() == ErrorCode::ResourceExhausted) {
        static_cast<void>(state_->close(CloseReason::SequenceExhausted));
    }
    return sent;
}

void ClientTransport::disconnect(Outbox& outbox) noexcept {
    if (state_->phase == Phase::Closed) {
        return;
    }
    if (state_->phase == Phase::Connected) {
        for (u32 i = 0; i < kDisconnectRepeats; ++i) {
            // Niêm phong chỉ lỗi khi hết số thứ tự: khi đó không gửi được gì nữa.
            if (!state_->transmit(PayloadKind::Disconnect, {}, state_->last_sent, outbox)) {
                break;
            }
        }
    }
    static_cast<void>(state_->close(CloseReason::LocalDisconnect));
}

ClientEvent ClientTransport::update(const core::MonoTime now, Outbox& outbox) noexcept {
    State& state = *state_;
    switch (state.phase) {
        case Phase::SendingRequest:
        case Phase::SendingResponse:
            if (now - state.started_at >= kHandshakeTimeout) {
                return state.close(CloseReason::HandshakeTimeout);
            }
            if (now - state.last_handshake_send >= kHandshakeResendInterval) {
                state.send_handshake(now, outbox);
            }
            return {};
        case Phase::Connected:
            if (now - state.last_received >= kIdleTimeout) {
                return state.close(CloseReason::Timeout);
            }
            if (now - state.last_sent >= kKeepAliveInterval &&
                !state.transmit(PayloadKind::KeepAlive, {}, now, outbox)) {
                return state.close(CloseReason::SequenceExhausted);
            }
            return {};
        case Phase::Closed:
            return {};
    }
    return {};
}

ClientState ClientTransport::state() const noexcept {
    switch (state_->phase) {
        case Phase::SendingRequest:
        case Phase::SendingResponse:
            return ClientState::Connecting;
        case Phase::Connected:
            return ClientState::Connected;
        case Phase::Closed:
            return ClientState::Closed;
    }
    return ClientState::Closed;
}

CloseReason ClientTransport::close_reason() const noexcept {
    return state_->close_reason;
}

std::optional<RejectReason> ClientTransport::reject_reason() const noexcept {
    return state_->reject_reason;
}

u32 ClientTransport::connection_id() const noexcept {
    return state_->channel.has_value() ? state_->channel->connection_id() : 0;
}

}  // namespace orion::net
