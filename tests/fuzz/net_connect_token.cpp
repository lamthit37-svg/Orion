// Fuzz VerifiedConnectToken::verify và inspect_connect_token (CLAUDE.md X.4, X.9;
// docs/formats/connect_token.md): token đến từ mạng trong gói bắt tay. Fuzz không giả được chữ ký,
// nên harness tự ký như io_manifest. Input: byte đầu chọn cách dựng, 8 byte kế là `now` (i64
// little-endian, micro giây), phần còn lại là token:
//   bit 0: ghi khoá công khai auth của harness vào trường signer (byte 100..131);
//   bit 1: ghi khoá định danh server của harness vào trường server_key (byte 68..99);
//   bit 2: ghi đè 64 byte cuối bằng chữ ký của harness trên 132 byte đầu.
// Nhờ vậy fuzz đi được tới các bước sau bước kiểm chữ ký. Tính chất: mọi input cho ra một token
// được nhận hoặc một lỗi trong bảng "Lỗi"; token được nhận thì đúng mọi luật, inspect đọc ra đúng
// các trường đó, và ký lại các trường đó cho đúng từng byte của input (mỗi token chỉ có một cách
// mã hoá).

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/connect_token.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

[[nodiscard]] orion::crypto::SigningKeyPair key_from(const std::uint8_t fill) {
    ORION_VERIFY(orion::crypto::initialize().has_value(), "sodium_init thất bại");
    std::array<std::byte, orion::crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{fill});
    return orion::crypto::signing_key_pair_from_seed(orion::crypto::SigningSeed(seed));
}

void check_accepted(const orion::net::VerifiedConnectToken& verified,
                    const std::span<const std::byte> token, const orion::core::WallTime now,
                    const orion::crypto::SigningKeyPair& auth,
                    const orion::crypto::SigningKeyPair& server) {
    namespace crypto = orion::crypto;
    namespace net = orion::net;
    const net::ConnectTokenClaims& claims = verified.claims();
    const orion::i64 issued = claims.issued_at.unix_microseconds();
    const orion::i64 expires = claims.expires_at.unix_microseconds();
    ORION_VERIFY(crypto::equal_constant_time(verified.signer(), auth.public_key),
                 "nhận người ký không được tin");
    ORION_VERIFY(crypto::equal_constant_time(claims.server_key, server.public_key),
                 "nhận token của cụm server khác");
    ORION_VERIFY(claims.account_id != 0 && issued < expires, "nhận các trường sai luật");
    ORION_VERIFY(now.unix_microseconds() < expires, "nhận token hết hạn");

    const orion::Result<net::ConnectTokenClaims> seen = net::inspect_connect_token(token);
    ORION_VERIFY(seen.has_value() && seen->account_id == claims.account_id &&
                     seen->issued_at == claims.issued_at && seen->expires_at == claims.expires_at &&
                     crypto::equal_constant_time(seen->client_key, claims.client_key) &&
                     crypto::equal_constant_time(seen->server_key, claims.server_key),
                 "inspect đọc khác verify");
    const orion::Result<net::ConnectToken> reissued = net::issue_connect_token(claims, auth);
    ORION_VERIFY(reissued.has_value() && std::ranges::equal(reissued->bytes, token),
                 "token được nhận có hơn một cách mã hoá");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orion::ErrorCode;
    namespace core = orion::core;
    namespace crypto = orion::crypto;
    namespace net = orion::net;

    static const crypto::SigningKeyPair kAuth = key_from(0x5A);
    static const crypto::SigningKeyPair kServer = key_from(0xA5);
    constexpr std::size_t kPrefix = 1 + sizeof(orion::i64);
    if (size < kPrefix) {
        return 0;
    }
    const std::uint8_t mode = data[0];
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size);
    std::memcpy(input.data(), data, size);
    const core::WallTime now = core::WallTime::from_unix_microseconds(
        core::load_le<orion::i64>(std::span<const std::byte>(input).subspan<1, 8>()));
    std::vector<std::byte> token(input.begin() + kPrefix, input.end());
    if (token.size() == net::kConnectTokenSize) {
        const std::span<std::byte> bytes(token);
        if ((mode & 1U) != 0) {
            std::ranges::copy(kAuth.public_key.bytes, bytes.begin() + 100);
        }
        if ((mode & 2U) != 0) {
            std::ranges::copy(kServer.public_key.bytes, bytes.begin() + 68);
        }
        if ((mode & 4U) != 0) {
            const crypto::Signature signature = crypto::sign(bytes.first(132), kAuth.secret_key);
            std::ranges::copy(signature.bytes, bytes.begin() + 132);
        }
    }

    const std::array trusted = {kAuth.public_key};
    const orion::Result<net::VerifiedConnectToken> verified =
        net::VerifiedConnectToken::verify(token, trusted, kServer.public_key, now);
    if (!verified.has_value()) {
        const ErrorCode code = verified.error().code();
        ORION_VERIFY(code == ErrorCode::InvalidArgument || code == ErrorCode::Unimplemented ||
                         code == ErrorCode::Unauthenticated ||
                         code == ErrorCode::PermissionDenied ||
                         code == ErrorCode::FailedPrecondition ||
                         code == ErrorCode::DeadlineExceeded || code == ErrorCode::DataLoss,
                     "mã lỗi ngoài bảng của định dạng");
        // inspect không kiểm chữ ký nhưng vẫn là hàm toàn phần.
        const orion::Result<net::ConnectTokenClaims> seen = net::inspect_connect_token(token);
        ORION_VERIFY(seen.has_value() || seen.error().code() == ErrorCode::InvalidArgument ||
                         seen.error().code() == ErrorCode::Unimplemented ||
                         seen.error().code() == ErrorCode::DataLoss,
                     "inspect trả mã lỗi ngoài bảng");
        return 0;
    }
    check_accepted(*verified, token, now, kAuth, kServer);
    return 0;
}
