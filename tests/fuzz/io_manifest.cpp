// Fuzz VerifiedManifest::verify (CLAUDE.md X.4, X.9; docs/formats/manifest.md): manifest tải từ
// dịch vụ patch hay nằm trên đĩa người chơi là dữ liệu từ ngoài. Fuzz không giả được chữ ký, nên
// harness tự ký. Byte đầu của input chọn cách dựng, phần còn lại là manifest:
//   bit 0: ghi khoá công khai của harness vào trường signer (byte 32..63);
//   bit 1: ghi đè 64 byte cuối bằng chữ ký của harness trên phần đứng trước.
// Nhờ vậy fuzz đi được tới phần đọc sau bước kiểm chữ ký. Mọi input cho ra một manifest được nhận
// hoặc một lỗi trong bảng "Lỗi"; manifest được nhận thì đúng mọi luật của định dạng.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/io/manifest.hpp"
#include "engine/io/pak.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

[[nodiscard]] orion::crypto::SigningKeyPair harness_key() {
    ORION_VERIFY(orion::crypto::initialize().has_value(), "sodium_init thất bại");
    std::array<std::byte, orion::crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{0x5A});
    return orion::crypto::signing_key_pair_from_seed(orion::crypto::SigningSeed(seed));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orion::ErrorCode;
    using orion::usize;
    namespace crypto = orion::crypto;
    namespace io = orion::io;

    static const crypto::SigningKeyPair kKey = harness_key();
    if (size == 0) {
        return 0;
    }
    const std::uint8_t mode = data[0];
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> bytes(size - 1);
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), data + 1, bytes.size());
    }
    if ((mode & 1U) != 0 && bytes.size() >= io::kManifestHeaderSize) {
        std::ranges::copy(kKey.public_key.bytes, bytes.begin() + 32);
    }
    if ((mode & 2U) != 0 && bytes.size() >= crypto::kSignatureSize) {
        const usize signed_size = bytes.size() - crypto::kSignatureSize;
        const crypto::Signature signature =
            crypto::sign(std::span(bytes).first(signed_size), kKey.secret_key);
        std::ranges::copy(signature.bytes, std::span(bytes).subspan(signed_size).begin());
    }

    const std::array trusted = {kKey.public_key};
    const orion::Result<io::VerifiedManifest> manifest =
        io::VerifiedManifest::verify(bytes, trusted, "win64");
    if (!manifest.has_value()) {
        const ErrorCode code = manifest.error().code();
        ORION_VERIFY(code == ErrorCode::InvalidArgument || code == ErrorCode::Unimplemented ||
                         code == ErrorCode::Unauthenticated ||
                         code == ErrorCode::FailedPrecondition || code == ErrorCode::DataLoss,
                     "mã lỗi ngoài bảng của định dạng");
        return 0;
    }
    const std::span<const io::ManifestPak> paks = manifest->paks();
    ORION_VERIFY(manifest->platform() == "win64", "nhận manifest của nền tảng khác");
    ORION_VERIFY(crypto::equal_constant_time(manifest->signer(), kKey.public_key),
                 "nhận người ký không được tin");
    ORION_VERIFY(paks.size() <= io::kMaxManifestPaks &&
                     bytes.size() == io::kManifestHeaderSize +
                                         (paks.size() * io::kManifestPakRecordSize) +
                                         crypto::kSignatureSize,
                 "số pak không khớp cỡ");
    for (usize i = 0; i < paks.size(); ++i) {
        ORION_VERIFY(paks[i].file_size >= io::kPakHeaderSize, "pak nhỏ hơn header");
        for (usize j = 0; j < i; ++j) {
            ORION_VERIFY(!crypto::equal_constant_time(paks[i].file_hash, paks[j].file_hash),
                         "hai pak trùng hash");
        }
    }
    return 0;
}
