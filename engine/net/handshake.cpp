#include "engine/net/handshake.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace orion::net {
namespace {

// Vị trí các trường (docs/formats/transport.md, mục Bắt tay).
constexpr usize kRequestVersionOffset = 1;
constexpr usize kRequestNonceOffset = 5;
constexpr usize kRequestTokenOffset = 21;
constexpr usize kRequestPaddingOffset = kRequestTokenOffset + kConnectTokenSize;
constexpr usize kChallengeNonceOffset = 1;
constexpr usize kChallengeCookieOffset = 17;
constexpr usize kResponseVersionOffset = 1;
constexpr usize kResponseNonceOffset = 5;
constexpr usize kResponseCookieOffset = 21;
constexpr usize kResponseTokenOffset = 53;
constexpr usize kAcceptNonceOffset = 1;
constexpr usize kAcceptConnectionIdOffset = 17;
constexpr usize kAcceptKeyOffset = 21;
constexpr usize kAcceptSignatureOffset = 53;
constexpr usize kRejectNonceOffset = 1;
constexpr usize kRejectReasonOffset = 17;
static_assert(kRequestPaddingOffset == 217);
static_assert(kChallengeCookieOffset + kCookieSize == kChallengeSize);
static_assert(kResponseTokenOffset + kConnectTokenSize == kResponseSize);
static_assert(kAcceptSignatureOffset + crypto::kSignatureSize == kAcceptSize);
static_assert(kRejectReasonOffset + 1 == kRejectSize);

// Cookie: số thứ tự rồi AEAD của expires_at (mục Cookie).
constexpr u32 kCookieNonceChannel = 1;
constexpr usize kCookieSequenceSize = sizeof(u64);
constexpr usize kCookieSealedSize = sizeof(i64) + crypto::kAeadTagSize;
static_assert(kCookieSequenceSize + kCookieSealedSize == kCookieSize);
// Địa chỉ trong associated data: họ, 16 byte địa chỉ, cổng.
constexpr usize kAddressFieldSize = 1 + 16 + 2;
constexpr usize kCookieAdSize = 4 + kHandshakeNonceSize + crypto::kHashSize + kAddressFieldSize;

// Bản ghi mà server ký trên ACCEPT (mục ACCEPT).
constexpr std::array<std::byte, 16> kAcceptContext = {
    std::byte{'O'}, std::byte{'R'}, std::byte{'I'}, std::byte{'O'}, std::byte{'N'}, std::byte{' '},
    std::byte{'A'}, std::byte{'C'}, std::byte{'C'}, std::byte{'E'}, std::byte{'P'}, std::byte{'T'},
    std::byte{' '}, std::byte{'v'}, std::byte{'1'}, std::byte{0}};
constexpr usize kAcceptTranscriptSize = kAcceptContext.size() + 4 + kHandshakeNonceSize +
                                        crypto::kHashSize + 4 + crypto::kKeyExchangePublicKeySize;
static_assert(kAcceptTranscriptSize == 104);

template <usize N, class Tag>
[[nodiscard]] crypto::PublicBytes<N, Tag> read_bytes(const std::span<const std::byte> packet,
                                                     const usize offset) noexcept {
    crypto::PublicBytes<N, Tag> value;
    std::ranges::copy(packet.subspan(offset, N), value.bytes.begin());
    return value;
}

template <usize N, class Tag>
void write_bytes(const std::span<std::byte> packet, const usize offset,
                 const crypto::PublicBytes<N, Tag>& value) noexcept {
    std::ranges::copy(value.bytes, packet.subspan(offset, N).begin());
}

// Bước chung của mọi gói bắt tay: đúng cỡ và prefix (4 bit cao bằng 0).
[[nodiscard]] Result<void> check_shape(const std::span<const std::byte> packet,
                                       const PacketType type, const usize size) noexcept {
    if (packet.size() != size || packet[0] != static_cast<std::byte>(std::to_underlying(type))) {
        return fail(ErrorCode::InvalidArgument, "net: gói bắt tay sai cỡ hay prefix",
                    static_cast<i64>(packet.size()));
    }
    return {};
}

[[nodiscard]] std::array<std::byte, kAddressFieldSize> address_field(
    const Address& address) noexcept {
    std::array<std::byte, kAddressFieldSize> field{};
    field[0] = static_cast<std::byte>(std::to_underlying(address.family()));
    std::ranges::transform(address.bytes(), field.begin() + 1,
                           [](const u8 b) { return std::byte{b}; });
    core::store_le<u16>(std::span(field).subspan<17, 2>(), address.port());
    return field;
}

[[nodiscard]] std::array<std::byte, kCookieAdSize> cookie_ad(const u32 protocol_version,
                                                             const HandshakeNonce& nonce,
                                                             const crypto::Hash& token_hash,
                                                             const Address& from) noexcept {
    std::array<std::byte, kCookieAdSize> ad{};
    core::ByteWriter writer(ad);
    writer.write<u32>(protocol_version);
    writer.write_bytes(nonce.view());
    writer.write_bytes(token_hash.view());
    writer.write_bytes(address_field(from));
    ORION_VERIFY(!writer.overflowed() && writer.offset() == ad.size(), "net: tính sai cỡ cookie");
    return ad;
}

[[nodiscard]] std::array<std::byte, kAcceptTranscriptSize> accept_transcript(
    const u32 protocol_version, const HandshakeNonce& nonce, const crypto::Hash& token_hash,
    const u32 connection_id, const crypto::KeyExchangePublicKey& server_key_exchange) noexcept {
    std::array<std::byte, kAcceptTranscriptSize> transcript{};
    core::ByteWriter writer(transcript);
    writer.write_bytes(kAcceptContext);
    writer.write<u32>(protocol_version);
    writer.write_bytes(nonce.view());
    writer.write_bytes(token_hash.view());
    writer.write<u32>(connection_id);
    writer.write_bytes(server_key_exchange.view());
    ORION_VERIFY(!writer.overflowed() && writer.offset() == transcript.size(),
                 "net: tính sai cỡ bản ghi ACCEPT");
    return transcript;
}

}  // namespace

std::array<std::byte, kRequestSize> write_request(const Request& request) noexcept {
    std::array<std::byte, kRequestSize> packet{};
    packet[0] = static_cast<std::byte>(std::to_underlying(PacketType::Request));
    core::store_le<u32>(std::span(packet).subspan<kRequestVersionOffset, 4>(),
                        request.protocol_version);
    write_bytes(packet, kRequestNonceOffset, request.nonce);
    write_bytes(packet, kRequestTokenOffset, request.token);
    return packet;
}

std::array<std::byte, kChallengeSize> write_challenge(const Challenge& challenge) noexcept {
    std::array<std::byte, kChallengeSize> packet{};
    packet[0] = static_cast<std::byte>(std::to_underlying(PacketType::Challenge));
    write_bytes(packet, kChallengeNonceOffset, challenge.nonce);
    write_bytes(packet, kChallengeCookieOffset, challenge.cookie);
    return packet;
}

std::array<std::byte, kResponseSize> write_response(const Response& response) noexcept {
    std::array<std::byte, kResponseSize> packet{};
    packet[0] = static_cast<std::byte>(std::to_underlying(PacketType::Response));
    core::store_le<u32>(std::span(packet).subspan<kResponseVersionOffset, 4>(),
                        response.protocol_version);
    write_bytes(packet, kResponseNonceOffset, response.nonce);
    write_bytes(packet, kResponseCookieOffset, response.cookie);
    write_bytes(packet, kResponseTokenOffset, response.token);
    return packet;
}

std::array<std::byte, kAcceptSize> write_accept(const Accept& accept) noexcept {
    std::array<std::byte, kAcceptSize> packet{};
    packet[0] = static_cast<std::byte>(std::to_underlying(PacketType::Accept));
    write_bytes(packet, kAcceptNonceOffset, accept.nonce);
    core::store_le<u32>(std::span(packet).subspan<kAcceptConnectionIdOffset, 4>(),
                        accept.connection_id);
    write_bytes(packet, kAcceptKeyOffset, accept.server_key_exchange);
    write_bytes(packet, kAcceptSignatureOffset, accept.signature);
    return packet;
}

std::array<std::byte, kRejectSize> write_reject(const Reject& reject) noexcept {
    std::array<std::byte, kRejectSize> packet{};
    packet[0] = static_cast<std::byte>(std::to_underlying(PacketType::Reject));
    write_bytes(packet, kRejectNonceOffset, reject.nonce);
    packet[kRejectReasonOffset] = static_cast<std::byte>(std::to_underlying(reject.reason));
    return packet;
}

Result<Request> read_request(const std::span<const std::byte> packet) noexcept {
    if (const Result<void> shape = check_shape(packet, PacketType::Request, kRequestSize); !shape) {
        return std::unexpected(shape.error());
    }
    if (!std::ranges::all_of(packet.subspan(kRequestPaddingOffset),
                             [](const std::byte b) { return b == std::byte{0}; })) {
        return fail(ErrorCode::InvalidArgument, "net: đệm của REQUEST khác 0");
    }
    Request request;
    request.protocol_version =
        core::load_le<u32>(packet.subspan<kRequestVersionOffset, sizeof(u32)>());
    request.nonce = read_bytes<kHandshakeNonceSize, HandshakeNonceTag>(packet, kRequestNonceOffset);
    request.token = read_bytes<kConnectTokenSize, ConnectTokenTag>(packet, kRequestTokenOffset);
    return request;
}

Result<Challenge> read_challenge(const std::span<const std::byte> packet) noexcept {
    if (const Result<void> shape = check_shape(packet, PacketType::Challenge, kChallengeSize);
        !shape) {
        return std::unexpected(shape.error());
    }
    Challenge challenge;
    challenge.nonce =
        read_bytes<kHandshakeNonceSize, HandshakeNonceTag>(packet, kChallengeNonceOffset);
    challenge.cookie = read_bytes<kCookieSize, CookieTag>(packet, kChallengeCookieOffset);
    return challenge;
}

Result<Response> read_response(const std::span<const std::byte> packet) noexcept {
    if (const Result<void> shape = check_shape(packet, PacketType::Response, kResponseSize);
        !shape) {
        return std::unexpected(shape.error());
    }
    Response response;
    response.protocol_version =
        core::load_le<u32>(packet.subspan<kResponseVersionOffset, sizeof(u32)>());
    response.nonce =
        read_bytes<kHandshakeNonceSize, HandshakeNonceTag>(packet, kResponseNonceOffset);
    response.cookie = read_bytes<kCookieSize, CookieTag>(packet, kResponseCookieOffset);
    response.token = read_bytes<kConnectTokenSize, ConnectTokenTag>(packet, kResponseTokenOffset);
    return response;
}

Result<Accept> read_accept(const std::span<const std::byte> packet) noexcept {
    if (const Result<void> shape = check_shape(packet, PacketType::Accept, kAcceptSize); !shape) {
        return std::unexpected(shape.error());
    }
    Accept accept;
    accept.connection_id =
        core::load_le<u32>(packet.subspan<kAcceptConnectionIdOffset, sizeof(u32)>());
    if (accept.connection_id == 0) {
        return fail(ErrorCode::InvalidArgument, "net: connection id của ACCEPT là 0");
    }
    accept.nonce = read_bytes<kHandshakeNonceSize, HandshakeNonceTag>(packet, kAcceptNonceOffset);
    accept.server_key_exchange =
        read_bytes<crypto::kKeyExchangePublicKeySize, crypto::KeyExchangePublicKeyTag>(
            packet, kAcceptKeyOffset);
    accept.signature =
        read_bytes<crypto::kSignatureSize, crypto::SignatureTag>(packet, kAcceptSignatureOffset);
    return accept;
}

Result<Reject> read_reject(const std::span<const std::byte> packet) noexcept {
    if (const Result<void> shape = check_shape(packet, PacketType::Reject, kRejectSize); !shape) {
        return std::unexpected(shape.error());
    }
    const auto reason = std::to_integer<u8>(packet[kRejectReasonOffset]);
    if (reason < std::to_underlying(RejectReason::VersionMismatch) ||
        reason > std::to_underlying(RejectReason::ServerFull)) {
        return fail(ErrorCode::InvalidArgument, "net: lý do của REJECT ngoài bảng", reason);
    }
    Reject reject;
    reject.nonce = read_bytes<kHandshakeNonceSize, HandshakeNonceTag>(packet, kRejectNonceOffset);
    reject.reason = static_cast<RejectReason>(reason);
    return reject;
}

crypto::Hash token_hash(const ConnectToken& token) noexcept {
    return crypto::hash(token.view());
}

crypto::Signature sign_accept(const u32 protocol_version, const HandshakeNonce& nonce,
                              const crypto::Hash& token_hash, const u32 connection_id,
                              const crypto::KeyExchangePublicKey& server_key_exchange,
                              const crypto::SigningSecretKey& identity) noexcept {
    return crypto::sign(
        accept_transcript(protocol_version, nonce, token_hash, connection_id, server_key_exchange),
        identity);
}

bool verify_accept(const Accept& accept, const u32 protocol_version, const crypto::Hash& token_hash,
                   const crypto::SigningPublicKey& identity) noexcept {
    return crypto::verify(accept_transcript(protocol_version, accept.nonce, token_hash,
                                            accept.connection_id, accept.server_key_exchange),
                          accept.signature, identity);
}

CookieJar::CookieJar(const core::MonoTime now) noexcept
    : current_(crypto::generate_aead_key()), rotated_at_(now) {}

Cookie CookieJar::issue(const u32 protocol_version, const HandshakeNonce& nonce,
                        const crypto::Hash& token_hash, const Address& from,
                        const core::MonoTime now) noexcept {
    if (now - rotated_at_ >= kCookieKeyLifetime) {
        previous_ = std::move(current_);
        current_ = crypto::generate_aead_key();
        has_previous_ = true;
        next_sequence_ = 0;
        rotated_at_ = now;
    }
    const u64 sequence = next_sequence_++;
    std::array<std::byte, sizeof(i64)> expires{};
    core::store_le<i64>(expires, (now + kCookieLifetime).nanoseconds());
    Cookie cookie;
    const std::span<std::byte, kCookieSize> bytes = cookie.bytes;
    core::store_le<u64>(bytes.first<kCookieSequenceSize>(), sequence);
    const Result<usize> sealed =
        crypto::seal(bytes.subspan<kCookieSequenceSize>(), expires,
                     cookie_ad(protocol_version, nonce, token_hash, from),
                     crypto::nonce_from_sequence(kCookieNonceChannel, sequence), current_);
    ORION_VERIFY(sealed.has_value() && *sealed == kCookieSealedSize, "net: niêm phong cookie lỗi");
    return cookie;
}

bool CookieJar::check(const Cookie& cookie, const u32 protocol_version, const HandshakeNonce& nonce,
                      const crypto::Hash& token_hash, const Address& from,
                      const core::MonoTime now) const noexcept {
    const std::span<const std::byte, kCookieSize> bytes = cookie.bytes;
    const auto sequence = core::load_le<u64>(bytes.first<kCookieSequenceSize>());
    const std::array<std::byte, kCookieAdSize> ad =
        cookie_ad(protocol_version, nonce, token_hash, from);
    const crypto::AeadNonce aead_nonce = crypto::nonce_from_sequence(kCookieNonceChannel, sequence);
    std::array<std::byte, sizeof(i64)> expires{};
    bool opened =
        crypto::open(expires, bytes.subspan<kCookieSequenceSize>(), ad, aead_nonce, current_)
            .has_value();
    if (!opened && has_previous_) {
        opened =
            crypto::open(expires, bytes.subspan<kCookieSequenceSize>(), ad, aead_nonce, previous_)
                .has_value();
    }
    return opened && now.nanoseconds() < core::load_le<i64>(expires);
}

}  // namespace orion::net
