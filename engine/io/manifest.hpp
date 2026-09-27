#pragma once

// Manifest đã ký (docs/formats/manifest.md): danh sách pak của một bản build, có chữ ký Ed25519 của
// khoá phát hành. Bản ship chỉ mount pak có trong một VerifiedManifest (CLAUDE.md X.9); kiểu này
// chỉ dựng được qua VerifiedManifest::verify, nên có nó trong tay là manifest đã qua kiểm chữ ký.
//
// Ghi và ký manifest (build_manifest) chỉ có khi ORION_DEV_TOOLS.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/sign.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orion::io {

inline constexpr std::array<std::byte, 8> kManifestMagic = {
    std::byte{'O'}, std::byte{'R'}, std::byte{'I'}, std::byte{'O'},
    std::byte{'N'}, std::byte{'M'}, std::byte{'A'}, std::byte{'N'}};
inline constexpr u16 kManifestVersion = 1;
inline constexpr usize kManifestHeaderSize = 64;
inline constexpr usize kManifestPakRecordSize = 72;
inline constexpr usize kManifestPlatformSize = 8;
inline constexpr u32 kMaxManifestPaks = 4096;

// Một pak mà manifest liệt kê.
struct ManifestPak {
    u64 file_size = 0;
    crypto::Hash file_hash;
    crypto::Hash index_hash;
};

// Tên nền tảng của manifest: 1 tới 8 ký tự a-z0-9 (win64, android, ios, linux).
[[nodiscard]] bool is_valid_platform(std::string_view platform) noexcept;

// Tên tệp của pak trong thư mục data/: hash của cả tệp viết hex thường, cộng ".pak".
[[nodiscard]] std::string pak_file_name(const crypto::Hash& file_hash);

class VerifiedManifest {
public:
    // Kiểm `bytes` theo docs/formats/manifest.md: đúng cấu trúc, người ký nằm trong
    // `trusted_signers`, chữ ký đúng, nền tảng bằng `platform` (phải đúng luật is_valid_platform).
    // Hàm toàn phần trên `bytes`; lỗi theo bảng "Lỗi" của định dạng.
    [[nodiscard]] static Result<VerifiedManifest> verify(
        std::span<const std::byte> bytes, std::span<const crypto::SigningPublicKey> trusted_signers,
        std::string_view platform) noexcept;

    [[nodiscard]] u64 sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::string_view platform() const noexcept {
        return {platform_.data(), platform_size_};
    }
    [[nodiscard]] const crypto::SigningPublicKey& signer() const noexcept { return signer_; }
    // Theo thứ tự mount: pak đứng sau che pak đứng trước.
    [[nodiscard]] std::span<const ManifestPak> paks() const noexcept { return paks_; }

private:
    VerifiedManifest() = default;

    u64 sequence_ = 0;
    std::array<char, kManifestPlatformSize> platform_{};
    usize platform_size_ = 0;
    crypto::SigningPublicKey signer_;
    std::vector<ManifestPak> paks_;
};

#if ORION_DEV_TOOLS
// Ghi và ký một manifest cho tool phát hành và test. Cùng đầu vào luôn cho cùng dãy byte (Ed25519
// tất định). Lỗi: InvalidArgument khi `platform` sai luật, quá kMaxManifestPaks pak, file_size nhỏ
// hơn header của pak, hay hai pak trùng file_hash.
[[nodiscard]] Result<std::vector<std::byte>> build_manifest(u64 sequence, std::string_view platform,
                                                            std::span<const ManifestPak> paks,
                                                            const crypto::SigningKeyPair& signer);
#endif

}  // namespace orion::io
