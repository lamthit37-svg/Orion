// Fuzz gói bắt tay, cookie và chữ ký ACCEPT (CLAUDE.md X.4, X.9; docs/formats/transport.md, mục Bắt
// tay): server đọc REQUEST và RESPONSE từ bất kỳ ai, client đọc CHALLENGE, ACCEPT, REJECT. Byte đầu
// chọn cách dùng phần còn lại:
//   0..4: gói REQUEST, CHALLENGE, RESPONSE, ACCEPT, REJECT. Bộ đọc là hàm toàn phần, chỉ trả
//         InvalidArgument; gói đọc được thì ghi lại cho đúng từng byte (một cách mã hoá cho mỗi
//         gói).
//   5: cookie do fuzz tạo không bao giờ qua check; cookie jar của harness cấp cho các trường lấy từ
//      input thì qua, và hỏng khi một trường bị đổi theo mặt nạ trong input.
//   6: ACCEPT do fuzz tạo không bao giờ qua verify_accept; chữ ký của harness trên các trường lấy
//      từ input thì qua, và hỏng khi một trường bị đổi.
//   7: như 0..4 nhưng chọn bộ đọc bằng packet_type.

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/handshake.hpp"
#include "engine/net/packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

using orion::u32;
using orion::u8;
using orion::usize;
namespace net = orion::net;
namespace crypto = orion::crypto;

[[nodiscard]] crypto::SigningKeyPair harness_identity() {
    ORION_VERIFY(crypto::initialize().has_value(), "sodium_init thất bại");
    std::array<std::byte, crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{0xA5});
    return crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
}

template <class Packet, class Reader, class Writer>
void check_round_trip(const std::span<const std::byte> input, const Reader& read,
                      const Writer& write) {
    const orion::Result<Packet> packet = read(input);
    if (!packet.has_value()) {
        ORION_VERIFY(packet.error().code() == orion::ErrorCode::InvalidArgument,
                     "bộ đọc gói bắt tay trả mã lỗi ngoài bảng");
        return;
    }
    const auto written = write(*packet);
    ORION_VERIFY(std::ranges::equal(written, input), "gói bắt tay có hơn một cách mã hoá");
}

void fuzz_reader(const u8 type, const std::span<const std::byte> input) {
    switch (type) {
        case 1:
            check_round_trip<net::Request>(input, net::read_request, net::write_request);
            break;
        case 2:
            check_round_trip<net::Challenge>(input, net::read_challenge, net::write_challenge);
            break;
        case 3:
            check_round_trip<net::Response>(input, net::read_response, net::write_response);
            break;
        case 4:
            check_round_trip<net::Accept>(input, net::read_accept, net::write_accept);
            break;
        default:
            check_round_trip<net::Reject>(input, net::read_reject, net::write_reject);
            break;
    }
}

// Các trường của một lần thử lấy từ đầu input: version, nonce, token hash, địa chỉ.
struct Attempt {
    u32 version = 0;
    net::HandshakeNonce nonce;
    crypto::Hash hash;
    net::Address from;
};

[[nodiscard]] Attempt attempt_from(const std::span<const std::byte, 4 + 16 + 32 + 7> bytes) {
    Attempt attempt;
    attempt.version = orion::core::load_le<u32>(bytes.first<4>());
    std::ranges::copy(bytes.subspan<4, 16>(), attempt.nonce.bytes.begin());
    std::ranges::copy(bytes.subspan<20, 32>(), attempt.hash.bytes.begin());
    std::array<u8, 4> octets{};
    std::ranges::transform(bytes.subspan<52, 4>(), octets.begin(),
                           [](const std::byte b) { return std::to_integer<u8>(b); });
    attempt.from =
        net::Address::v4(octets, orion::core::load_le<orion::u16>(bytes.subspan<56, 2>()));
    return attempt;
}

void fuzz_cookie(const std::span<const std::byte> input) {
    constexpr usize kFields = 4 + 16 + 32 + 7;
    if (input.size() < kFields + net::kCookieSize) {
        return;
    }
    const orion::core::MonoTime now = orion::core::MonoTime::from_nanoseconds(1'000'000'000);
    net::CookieJar jar(now);
    const Attempt attempt = attempt_from(input.first<kFields>());
    net::Cookie forged;
    std::ranges::copy(input.subspan(kFields, net::kCookieSize), forged.bytes.begin());
    ORION_VERIFY(
        !jar.check(forged, attempt.version, attempt.nonce, attempt.hash, attempt.from, now),
        "cookie không do jar cấp mà qua check");
    const net::Cookie issued =
        jar.issue(attempt.version, attempt.nonce, attempt.hash, attempt.from, now);
    ORION_VERIFY(jar.check(issued, attempt.version, attempt.nonce, attempt.hash, attempt.from, now),
                 "cookie vừa cấp không qua check");
    // Mặt nạ đổi các trường: byte ngay sau cookie do fuzz tạo, nếu có.
    if (input.size() > kFields + net::kCookieSize) {
        const auto mask = std::to_integer<u8>(input[kFields + net::kCookieSize]);
        if (mask != 0) {
            net::HandshakeNonce nonce = attempt.nonce;
            nonce.bytes[mask % net::kHandshakeNonceSize] ^= std::byte{mask};
            ORION_VERIFY(
                !jar.check(issued, attempt.version, nonce, attempt.hash, attempt.from, now),
                "cookie qua check với nonce khác");
            ORION_VERIFY(!jar.check(issued, attempt.version ^ mask, attempt.nonce, attempt.hash,
                                    attempt.from, now),
                         "cookie qua check với version khác");
        }
    }
}

void fuzz_accept(const std::span<const std::byte> input, const crypto::SigningKeyPair& identity) {
    const orion::Result<net::Accept> accept =
        net::read_accept(input.first(std::min(input.size(), net::kAcceptSize)));
    if (!accept.has_value()) {
        return;
    }
    const crypto::Hash hash = crypto::hash(input);
    ORION_VERIFY(!net::verify_accept(*accept, 1, hash, identity.public_key),
                 "ACCEPT do fuzz tạo mà qua verify_accept");
    net::Accept signed_accept = *accept;
    signed_accept.signature = net::sign_accept(1, accept->nonce, hash, accept->connection_id,
                                               accept->server_key_exchange, identity.secret_key);
    ORION_VERIFY(net::verify_accept(signed_accept, 1, hash, identity.public_key),
                 "ACCEPT harness ký mà không qua verify_accept");
    ORION_VERIFY(!net::verify_accept(signed_accept, 2, hash, identity.public_key),
                 "ACCEPT qua verify_accept với version khác");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const crypto::SigningKeyPair kIdentity = harness_identity();
    if (size == 0) {
        return 0;
    }
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size - 1);
    if (!input.empty()) {
        std::memcpy(input.data(), data + 1, input.size());
    }
    const u8 mode = data[0] & 7U;
    if (mode <= 4) {
        fuzz_reader(static_cast<u8>(mode + 1), input);
    } else if (mode == 5) {
        fuzz_cookie(input);
    } else if (mode == 6) {
        fuzz_accept(input, kIdentity);
    } else {
        const orion::Result<net::PacketType> type = net::packet_type(input);
        if (type.has_value() && *type != net::PacketType::Data) {
            fuzz_reader(static_cast<u8>(*type), input);
        }
    }
    return 0;
}
