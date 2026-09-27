#include "engine/net/server_transport.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/slot_map.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/short_hash.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/connection.hpp"
#include "engine/net/handshake.hpp"
#include "engine/net/outbox.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/rate_limit.hpp"
#include "engine/net/secure_channel.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orion::net {
namespace {

// Số lần thử chọn connection id: bảng id luôn còn ít nhất nửa ô trống, nên mỗi lần thử trúng ô
// trống với xác suất từ 1/2 trở lên.
constexpr u32 kConnectionIdAttempts = 64;
// Bảng token đã dùng giữ tối đa chừng này lần sức chứa kết nối; đầy thì từ chối kết nối mới
// (lý do 5) tới khi token cũ hết hạn. Chính sách, không phải số đo.
constexpr usize kTokenTableFactor = 4;
constexpr core::Duration kTokenPurgeInterval = core::Duration::seconds(1);

struct Connection {
    Connection(const u32 id, const Address& peer, crypto::SessionKeys keys,
               const crypto::Hash& hash, const HandshakeNonce& handshake_nonce, const u64 account,
               const std::array<std::byte, kAcceptSize>& accept_packet,
               const core::MonoTime now) noexcept
        : channel(id, std::move(keys)),
          address(peer),
          token_hash(hash),
          nonce(handshake_nonce),
          account_id(account),
          accept(accept_packet),
          created_at(now),
          last_received(now),
          last_sent(now) {}

    SecureChannel channel;
    Address address;
    crypto::Hash token_hash;
    HandshakeNonce nonce;
    u64 account_id;
    // Gửi lại đúng gói này khi RESPONSE lặp lại (ACCEPT bị mất).
    std::array<std::byte, kAcceptSize> accept;
    bool confirmed = false;
    core::MonoTime created_at;
    core::MonoTime last_received;
    core::MonoTime last_sent;
    // Gói dữ liệu đã xác thực của kết nối này (packet_limit_per_connection).
    TokenBucket packets;
};

using ConnectionMap = core::SlotMap<Connection>;
using ConnectionHandle = ConnectionMap::HandleType;

struct IdEntry {
    u32 id = 0;
    ConnectionHandle handle;
};

struct TokenKey {
    crypto::Hash hash;
};

struct TokenKeyHash {
    [[nodiscard]] usize operator()(const TokenKey& key) const noexcept {
        // BLAKE2b phân bố đều; token do auth ký nên kẻ ngoài không chọn được hash của nó.
        return static_cast<usize>(core::load_le<u64>(key.hash.view().first<8>()));
    }
};

struct TokenKeyEqual {
    [[nodiscard]] bool operator()(const TokenKey& a, const TokenKey& b) const noexcept {
        return crypto::equal_constant_time(a.hash, b.hash);
    }
};

struct TokenEntry {
    core::WallTime expires_at;
    // Kết nối đang dùng token; 0 khi kết nối đã đóng.
    u32 connection_id = 0;
};

[[nodiscard]] usize id_table_size(const u32 max_connections) noexcept {
    return std::bit_ceil(usize{max_connections} * 2);
}

}  // namespace

struct ServerTransport::State {
    State(ServerConfig server_config, const core::MonoTime now)
        : config(std::move(server_config)),
          responses_by_address(config.response_limit_per_address, config.address_slots,
                               config.address_hash_key.value_or(crypto::generate_short_hash_key())),
          cookies(now),
          connections(config.max_connections),
          ids(id_table_size(config.max_connections)),
          id_mask(static_cast<u32>(ids.size() - 1)),
          token_capacity(usize{config.max_connections} * kTokenTableFactor),
          last_purge(now) {
        tokens.reserve(token_capacity);
    }

    [[nodiscard]] Connection* find(const u32 id) noexcept {
        const IdEntry& entry = ids[id & id_mask];
        return (id != 0 && entry.id == id) ? connections.get(entry.handle) : nullptr;
    }

    [[nodiscard]] u32 allocate_id() noexcept {
        for (u32 attempt = 0; attempt < kConnectionIdAttempts; ++attempt) {
            std::array<std::byte, sizeof(u32)> random{};
            crypto::random_bytes(random);
            const u32 id = core::load_le<u32>(random);
            if (id != 0 && ids[id & id_mask].id == 0) {
                return id;
            }
        }
        return 0;
    }

    void remove(const u32 id) noexcept {
        IdEntry& entry = ids[id & id_mask];
        if (id == 0 || entry.id != id) {
            return;
        }
        if (const Connection* connection = connections.get(entry.handle); connection != nullptr) {
            const auto token = tokens.find(TokenKey{connection->token_hash});
            if (token != tokens.end() && token->second.connection_id == id) {
                token->second.connection_id = 0;
            }
        }
        static_cast<void>(connections.remove(entry.handle));
        entry = IdEntry{};
    }

    // Gửi một payload qua kết nối; lỗi của seal được trả lại cho bên gọi.
    [[nodiscard]] Result<void> transmit(Connection& connection, const PayloadKind kind,
                                        const std::span<const std::byte> data,
                                        const core::MonoTime now, Outbox& outbox) noexcept {
        const Result<usize> sealed = seal_payload(connection.channel, kind, data, packet);
        if (!sealed) {
            return std::unexpected(sealed.error());
        }
        static_cast<void>(outbox.push(connection.address, std::span(packet).first(*sealed)));
        connection.last_sent = now;
        return {};
    }

    void on_request(const Address& from, std::span<const std::byte> bytes, core::MonoTime now,
                    Outbox& outbox) noexcept;
    void on_response(const Address& from, std::span<const std::byte> bytes, core::MonoTime now,
                     core::WallTime wall_now, Outbox& outbox) noexcept;
    [[nodiscard]] ServerEvent on_data(const Address& from, std::span<const std::byte> bytes,
                                      core::MonoTime now) noexcept;
    // Bước 4 của RESPONSE: tạo kết nối chờ và ghi ACCEPT; lý do từ chối khi không được.
    [[nodiscard]] std::optional<RejectReason> accept(const Address& from, const Response& response,
                                                     const crypto::Hash& hash,
                                                     const VerifiedConnectToken& token,
                                                     core::MonoTime now, Outbox& outbox) noexcept;

    ServerConfig config;
    TokenBucket requests;
    TokenBucket responses;
    AddressRateLimiter responses_by_address;
    CookieJar cookies;
    ConnectionMap connections;
    // Bảng id ánh xạ thẳng: connection id được chọn sao cho ô id & id_mask còn trống.
    std::vector<IdEntry> ids;
    u32 id_mask;
    std::unordered_map<TokenKey, TokenEntry, TokenKeyHash, TokenKeyEqual> tokens;
    usize token_capacity;
    core::MonoTime last_purge;
    std::array<std::byte, kMaxPacketSize> plaintext{};
    std::array<std::byte, kMaxPacketSize> packet{};
};

void ServerTransport::State::on_request(const Address& from, const std::span<const std::byte> bytes,
                                        const core::MonoTime now, Outbox& outbox) noexcept {
    const Result<Request> request = read_request(bytes);
    if (!request || !requests.try_take(config.request_limit, now)) {
        return;
    }
    if (request->protocol_version != config.protocol_version) {
        static_cast<void>(outbox.push(
            from,
            write_reject({.nonce = request->nonce, .reason = RejectReason::VersionMismatch})));
        return;
    }
    const Cookie cookie = cookies.issue(request->protocol_version, request->nonce,
                                        token_hash(request->token), from, now);
    static_cast<void>(
        outbox.push(from, write_challenge({.nonce = request->nonce, .cookie = cookie})));
}

void ServerTransport::State::on_response(const Address& from,
                                         const std::span<const std::byte> bytes,
                                         const core::MonoTime now, const core::WallTime wall_now,
                                         Outbox& outbox) noexcept {
    const Result<Response> response = read_response(bytes);
    if (!response) {
        return;
    }
    const crypto::Hash hash = token_hash(response->token);
    // Cookie chỉ được cấp cho protocol_version của server, nên qua được bước này là version đúng.
    if (!cookies.check(response->cookie, response->protocol_version, response->nonce, hash, from,
                       now)) {
        return;  // Địa chỉ nguồn chưa được chứng minh: không trả lời gì.
    }
    // Cookie chứng minh địa chỉ nguồn, nên giới hạn theo địa chỉ không bị kẻ giả địa chỉ dùng để
    // chặn người khác. Hết lượt của địa chỉ thì không tốn lượt chung.
    if (!responses_by_address.try_take(from, now) ||
        !responses.try_take(config.response_limit, now)) {
        return;
    }
    const Result<VerifiedConnectToken> token = VerifiedConnectToken::verify(
        response->token.view(), config.token_signers, config.identity.public_key, wall_now);
    std::optional<RejectReason> reason;
    if (!token) {
        reason = token.error().code() == ErrorCode::DeadlineExceeded ? RejectReason::TokenExpired
                                                                     : RejectReason::TokenInvalid;
    } else {
        reason = accept(from, *response, hash, *token, now, outbox);
    }
    if (reason.has_value()) {
        static_cast<void>(
            outbox.push(from, write_reject({.nonce = response->nonce, .reason = *reason})));
    }
}

std::optional<RejectReason> ServerTransport::State::accept(
    const Address& from, const Response& response, const crypto::Hash& hash,
    const VerifiedConnectToken& token, const core::MonoTime now, Outbox& outbox) noexcept {
    if (const auto used = tokens.find(TokenKey{hash}); used != tokens.end()) {
        const Connection* pending = find(used->second.connection_id);
        if (pending != nullptr && !pending->confirmed && pending->address == from &&
            crypto::equal_constant_time(pending->nonce, response.nonce)) {
            static_cast<void>(outbox.push(from, pending->accept));  // ACCEPT trước bị mất.
            return std::nullopt;
        }
        return RejectReason::TokenUsed;
    }
    if (connections.size() >= config.max_connections || tokens.size() >= token_capacity) {
        return RejectReason::ServerFull;
    }
    const crypto::KeyExchangeKeyPair ephemeral = crypto::generate_key_exchange_key_pair();
    Result<crypto::SessionKeys> keys =
        crypto::server_session_keys(ephemeral, token.claims().client_key);
    if (!keys) {
        return RejectReason::TokenInvalid;  // Khoá X25519 của client là điểm bậc thấp.
    }
    const u32 id = allocate_id();
    if (id == 0) {
        return RejectReason::ServerFull;
    }
    const Accept accept_message{
        .nonce = response.nonce,
        .connection_id = id,
        .server_key_exchange = ephemeral.public_key,
        .signature = sign_accept(config.protocol_version, response.nonce, hash, id,
                                 ephemeral.public_key, config.identity.secret_key)};
    const Result<ConnectionHandle> handle = connections.insert(
        Connection(id, from, std::move(*keys), hash, response.nonce, token.claims().account_id,
                   write_accept(accept_message), now));
    ORION_VERIFY(handle.has_value(), "net: bảng kết nối đầy dù đã kiểm cỡ");
    ids[id & id_mask] = IdEntry{.id = id, .handle = *handle};
    tokens.emplace(TokenKey{hash},
                   TokenEntry{.expires_at = token.claims().expires_at, .connection_id = id});
    const Connection* created = connections.get(*handle);
    static_cast<void>(outbox.push(from, created->accept));
    return std::nullopt;
}

ServerEvent ServerTransport::State::on_data(const Address& from,
                                            const std::span<const std::byte> bytes,
                                            const core::MonoTime now) noexcept {
    const Result<DataHeader> header = read_data_header(bytes);
    if (!header) {
        return {};
    }
    Connection* connection = find(header->connection_id);
    if (connection == nullptr) {
        return {};
    }
    const std::optional<u64> highest = connection->channel.highest_received();
    const Result<usize> opened = connection->channel.open(plaintext, bytes);
    // Chỉ gói đã qua AEAD mới tốn lượt, nên kẻ không có khoá không làm hết lượt của kết nối.
    if (!opened || !connection->packets.try_take(config.packet_limit_per_connection, now)) {
        return {};
    }
    // Gói đã được xác thực. Chỉ gói mới nhất mới chuyển được địa chỉ (client đổi mạng hay NAT đổi
    // cổng); gói cũ hơn tới từ chỗ khác vẫn được nhận.
    connection->last_received = now;
    if (!highest.has_value() || header->sequence > *highest) {
        connection->address = from;
    }
    ServerEvent event;
    event.connection_id = header->connection_id;
    event.account_id = connection->account_id;
    if (!connection->confirmed) {
        connection->confirmed = true;
        event.connected = true;
    }
    const std::optional<Payload> payload = parse_payload(std::span(plaintext).first(*opened));
    if (!payload.has_value()) {
        return event;  // Gói đúng khoá nhưng sai luật: bỏ nội dung, giữ kết nối.
    }
    if (payload->kind == PayloadKind::Data) {
        event.payload = payload->data;
    } else if (payload->kind == PayloadKind::Disconnect) {
        remove(header->connection_id);
        event.disconnected = true;
    }
    return event;
}

ServerTransport::ServerTransport(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
ServerTransport::ServerTransport(ServerTransport&&) noexcept = default;
ServerTransport& ServerTransport::operator=(ServerTransport&&) noexcept = default;
ServerTransport::~ServerTransport() = default;

Result<ServerTransport> ServerTransport::create(ServerConfig config, const core::MonoTime now) {
    if (config.max_connections == 0 || config.token_signers.empty()) {
        return fail(ErrorCode::InvalidArgument, "net: server cần sức chứa và khoá của auth");
    }
    if (!config.request_limit.valid() || !config.response_limit_per_address.valid() ||
        !config.response_limit.valid() || !config.packet_limit_per_connection.valid() ||
        config.address_slots == 0 || config.address_slots > kMaxAddressLimiterSlots) {
        return fail(ErrorCode::InvalidArgument, "net: giới hạn tần suất của server sai");
    }
    return ServerTransport(std::make_unique<State>(std::move(config), now));
}

ServerEvent ServerTransport::receive(const Address& from, const std::span<const std::byte> packet,
                                     const core::MonoTime now, const core::WallTime wall_now,
                                     Outbox& outbox) noexcept {
    const Result<PacketType> type = packet_type(packet);
    if (!type) {
        return {};
    }
    switch (*type) {
        case PacketType::Request:
            state_->on_request(from, packet, now, outbox);
            return {};
        case PacketType::Response:
            state_->on_response(from, packet, now, wall_now, outbox);
            return {};
        case PacketType::Data:
            return state_->on_data(from, packet, now);
        case PacketType::Challenge:
        case PacketType::Accept:
        case PacketType::Reject:
            return {};  // Chỉ server gửi các gói này.
    }
    return {};
}

Result<void> ServerTransport::send(const u32 connection_id,
                                   const std::span<const std::byte> payload,
                                   const core::MonoTime now, Outbox& outbox) noexcept {
    Connection* connection = state_->find(connection_id);
    if (connection == nullptr || !connection->confirmed) {
        return fail(ErrorCode::NotFound, "net: không có kết nối đã xác nhận với id này");
    }
    const Result<void> sent =
        state_->transmit(*connection, PayloadKind::Data, payload, now, outbox);
    if (!sent && sent.error().code() == ErrorCode::ResourceExhausted) {
        state_->remove(connection_id);
    }
    return sent;
}

void ServerTransport::disconnect(const u32 connection_id, Outbox& outbox) noexcept {
    Connection* connection = state_->find(connection_id);
    if (connection == nullptr) {
        return;
    }
    for (u32 i = 0; i < kDisconnectRepeats; ++i) {
        // Niêm phong chỉ lỗi khi hết số thứ tự: khi đó không gửi được gì nữa.
        if (!state_->transmit(*connection, PayloadKind::Disconnect, {}, connection->last_sent,
                              outbox)) {
            break;
        }
    }
    state_->remove(connection_id);
}

usize ServerTransport::update(const core::MonoTime now, const core::WallTime wall_now,
                              Outbox& outbox, const std::span<u32> closed) noexcept {
    usize reported = 0;
    State& state = *state_;
    // Duyệt ngược: remove đổi phần tử cuối vào chỗ trống, nên các vị trí chưa duyệt không đổi.
    for (u32 position = state.connections.size(); position > 0; --position) {
        Connection& connection = state.connections.values()[position - 1];
        const u32 id = connection.channel.connection_id();
        const bool pending_expired =
            !connection.confirmed && now - connection.created_at >= kPendingTimeout;
        const bool idle_expired =
            connection.confirmed && now - connection.last_received >= kIdleTimeout;
        if (pending_expired) {
            state.remove(id);
        } else if (idle_expired) {
            if (reported == closed.size()) {
                continue;  // Để lần gọi sau báo.
            }
            closed[reported++] = id;
            state.remove(id);
        } else if (connection.confirmed && now - connection.last_sent >= kKeepAliveInterval) {
            if (!state.transmit(connection, PayloadKind::KeepAlive, {}, now, outbox)) {
                if (reported < closed.size()) {
                    closed[reported++] = id;
                    state.remove(id);
                }
            }
        }
    }
    if (now - state.last_purge >= kTokenPurgeInterval) {
        state.last_purge = now;
        std::erase_if(state.tokens, [&](const auto& entry) {
            return entry.second.connection_id == 0 && entry.second.expires_at <= wall_now;
        });
    }
    return reported;
}

u32 ServerTransport::connection_count() const noexcept {
    return state_->connections.size();
}

std::optional<Address> ServerTransport::address_of(const u32 connection_id) const noexcept {
    const Connection* connection = state_->find(connection_id);
    if (connection == nullptr) {
        return std::nullopt;
    }
    return connection->address;
}

}  // namespace orion::net
