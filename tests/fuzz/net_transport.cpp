// Fuzz hai máy trạng thái của transport (CLAUDE.md X.4, X.9; docs/formats/transport.md, mục Bắt tay
// và Kết nối): một server và một client thật nói chuyện qua mạng giả, xen với gói do fuzz tạo, gói
// thật bị sửa, gửi dữ liệu, ngắt kết nối và bước thời gian. Input là một kịch bản: mỗi bước là một
// byte lệnh rồi dữ liệu của lệnh đó.
//
// Bất biến: mỗi gói server ghi vào outbox trong một lần receive không lớn hơn gói nó vừa nhận
// (chống khuếch đại); số kết nối không vượt sức chứa; payload tầng trên không vượt
// kMaxTransportPayload. ASan và UBSan của preset fuzz bắt phần còn lại.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/client_transport.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/connection.hpp"
#include "engine/net/outbox.hpp"
#include "engine/net/server_transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace {

using orion::u32;
using orion::u8;
using orion::usize;
namespace core = orion::core;
namespace crypto = orion::crypto;
namespace net = orion::net;

constexpr u32 kVersion = 7;
constexpr u32 kMaxConnections = 2;
constexpr orion::i64 kIssuedAt = 1'790'000'000'000'000;
constexpr net::Address kServer = net::Address::v4({10, 0, 0, 1}, 7777);
constexpr std::array<net::Address, 3> kPeers = {net::Address::v4({192, 0, 2, 10}, 50'000),
                                                net::Address::v4({192, 0, 2, 11}, 50'001),
                                                net::Address::v4({198, 51, 100, 9}, 1)};

struct Keys {
    crypto::SigningKeyPair auth;
    crypto::SigningKeyPair identity;
    crypto::KeyExchangeKeyPair client;
    net::ConnectToken token;
};

[[nodiscard]] Keys make_keys() {
    ORION_VERIFY(crypto::initialize().has_value(), "sodium_init thất bại");
    std::array<std::byte, crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{0x11});
    Keys keys{.auth = crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed)),
              .identity = {},
              .client = {},
              .token = {}};
    seed.fill(std::byte{0x33});
    keys.identity = crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
    std::array<std::byte, crypto::kKeyExchangeSecretKeySize> secret{};
    secret.fill(std::byte{0x55});
    keys.client = crypto::key_exchange_key_pair_from_secret(crypto::KeyExchangeSecretKey(secret));
    net::ConnectTokenClaims claims;
    claims.account_id = 42;
    claims.issued_at = core::WallTime::from_unix_microseconds(kIssuedAt);
    claims.expires_at = claims.issued_at + core::Duration::seconds(120);
    claims.client_key = keys.client.public_key;
    claims.server_key = keys.identity.public_key;
    const orion::Result<net::ConnectToken> token = net::issue_connect_token(claims, keys.auth);
    ORION_VERIFY(token.has_value(), "không ký được token của harness");
    keys.token = *token;
    return keys;
}

// Đọc tuần tự kịch bản; hết byte thì mọi lần đọc trả 0 và rỗng.
class Script {
public:
    explicit Script(const std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool done() const noexcept { return bytes_.empty(); }
    [[nodiscard]] u8 byte() noexcept {
        if (bytes_.empty()) {
            return 0;
        }
        const auto value = std::to_integer<u8>(bytes_[0]);
        bytes_ = bytes_.subspan(1);
        return value;
    }
    [[nodiscard]] std::span<const std::byte> take(const usize count) noexcept {
        const usize n = std::min(count, bytes_.size());
        const std::span<const std::byte> taken = bytes_.first(n);
        bytes_ = bytes_.subspan(n);
        return taken;
    }

private:
    std::span<const std::byte> bytes_;
};

[[nodiscard]] net::ServerTransport make_server(const Keys& keys, const core::MonoTime now) {
    orion::Result<net::ServerTransport> server =
        net::ServerTransport::create({.identity = keys.identity,
                                      .token_signers = {keys.auth.public_key},
                                      .protocol_version = kVersion,
                                      .max_connections = kMaxConnections},
                                     now);
    ORION_VERIFY(server.has_value(), "không tạo được server của harness");
    return std::move(*server);
}

[[nodiscard]] net::ClientTransport make_client(const Keys& keys, const core::MonoTime now,
                                               net::Outbox& outbox) {
    orion::Result<net::ClientTransport> client =
        net::ClientTransport::connect(kServer, keys.token, keys.client, kVersion, now, outbox);
    ORION_VERIFY(client.has_value(), "không tạo được client của harness");
    return std::move(*client);
}

class Harness {
public:
    explicit Harness(const Keys& keys)
        : server_(make_server(keys, now_)), client_(make_client(keys, now_, client_out_)) {}

    void step(Script& script) {
        switch (script.byte() % 9) {
            case 0:
                flush_client(kPeers[script.byte() % kPeers.size()]);
                break;
            case 1:
                flush_server();
                break;
            case 2: {
                const net::Address from = kPeers[script.byte() % kPeers.size()];
                const std::span<const std::byte> packet = script.take(script.byte() * usize{5});
                to_server(from, packet);
                break;
            }
            case 3:
                to_client(script.take(script.byte() * usize{5}));
                break;
            case 4:
                mutate_and_resend(script);
                break;
            case 5:
                advance(core::Duration::milliseconds(i64_of(script.byte()) * 50));
                break;
            case 6:
                check_send(client_.send(script.take(usize{1} + script.byte()), now_, client_out_));
                break;
            case 7:
                check_send(server_.send(client_.connection_id(),
                                        script.take(usize{1} + script.byte()), now_, server_out_));
                break;
            default:
                if ((script.byte() & 1U) != 0) {
                    client_.disconnect(client_out_);
                } else {
                    server_.disconnect(client_.connection_id(), server_out_);
                }
                break;
        }
        ORION_VERIFY(server_.connection_count() <= kMaxConnections, "vượt sức chứa kết nối");
    }

private:
    [[nodiscard]] static orion::i64 i64_of(const u8 value) noexcept { return value; }

    // Gửi chỉ được hỏng vì lý do đã khai trong header: chưa kết nối, không có kết nối, sai cỡ, hết
    // số thứ tự.
    static void check_send(const orion::Result<void>& sent) {
        if (sent.has_value()) {
            return;
        }
        const orion::ErrorCode code = sent.error().code();
        ORION_VERIFY(code == orion::ErrorCode::FailedPrecondition ||
                         code == orion::ErrorCode::NotFound ||
                         code == orion::ErrorCode::InvalidArgument ||
                         code == orion::ErrorCode::ResourceExhausted,
                     "send hỏng với mã lỗi ngoài header");
    }

    void to_server(const net::Address& from, const std::span<const std::byte> packet) {
        const usize before = server_out_.packets().size();
        const net::ServerEvent event = server_.receive(from, packet, now_, wall_, server_out_);
        for (const net::OutgoingPacket& reply : server_out_.packets().subspan(before)) {
            ORION_VERIFY(reply.size <= packet.size(), "server trả lời lớn hơn gói nó nhận");
        }
        ORION_VERIFY(event.payload.size() <= net::kMaxTransportPayload, "payload quá lớn");
        last_to_server_.assign(packet.begin(), packet.end());
    }

    void to_client(const std::span<const std::byte> packet) {
        const net::ClientEvent event = client_.receive(kServer, packet, now_, client_out_);
        ORION_VERIFY(event.payload.size() <= net::kMaxTransportPayload, "payload quá lớn");
    }

    void flush_client(const net::Address& from) {
        const std::vector<net::OutgoingPacket> packets(client_out_.packets().begin(),
                                                       client_out_.packets().end());
        client_out_.clear();
        for (const net::OutgoingPacket& packet : packets) {
            to_server(from, packet.view());
        }
    }

    void flush_server() {
        const std::vector<net::OutgoingPacket> packets(server_out_.packets().begin(),
                                                       server_out_.packets().end());
        server_out_.clear();
        for (const net::OutgoingPacket& packet : packets) {
            to_client(packet.view());
        }
    }

    // Sửa một byte của gói gần nhất đã tới server rồi gửi lại từ một địa chỉ do kịch bản chọn.
    void mutate_and_resend(Script& script) {
        if (last_to_server_.empty()) {
            return;
        }
        std::vector<std::byte> packet = last_to_server_;
        const usize position = (usize{script.byte()} * 5) % packet.size();
        packet[position] ^= std::byte{script.byte()};
        to_server(kPeers[script.byte() % kPeers.size()], packet);
    }

    void advance(const core::Duration step) {
        now_ = now_ + step;
        wall_ = wall_ + step;
        static_cast<void>(client_.update(now_, client_out_));
        std::array<u32, kMaxConnections> closed{};
        static_cast<void>(server_.update(now_, wall_, server_out_, closed));
    }

    core::MonoTime now_ = core::MonoTime::from_nanoseconds(1'000'000'000'000);
    core::WallTime wall_ = core::WallTime::from_unix_microseconds(kIssuedAt + 1'000'000);
    net::Outbox server_out_{16};
    net::Outbox client_out_{16};
    net::ServerTransport server_;
    net::ClientTransport client_;
    std::vector<std::byte> last_to_server_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const Keys kKeys = make_keys();
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size);
    if (size != 0) {
        std::memcpy(input.data(), data, size);
    }
    Harness harness(kKeys);
    Script script(input);
    for (u32 steps = 0; steps < 256 && !script.done(); ++steps) {
        harness.step(script);
    }
    return 0;
}
