#include "engine/io/manifest.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/io/pak.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orion::io {
namespace {

// Vị trí các trường của header và của bản ghi pak (docs/formats/manifest.md).
constexpr usize kFlagsOffset = 10;
constexpr usize kPakCountOffset = 12;
constexpr usize kSequenceOffset = 16;
constexpr usize kPlatformOffset = 24;
constexpr usize kSignerOffset = 32;
constexpr usize kFileHashOffset = 8;
constexpr usize kIndexHashOffset = 40;

[[nodiscard]] bool is_platform_char(const char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Độ dài tên nền tảng trong trường 8 byte (ký tự a-z0-9 rồi toàn 0), hoặc 0 khi sai luật.
[[nodiscard]] usize platform_length(
    const std::span<const std::byte, kManifestPlatformSize> field) noexcept {
    const auto zero = std::ranges::find(field, std::byte{0});
    const auto length = static_cast<usize>(zero - field.begin());
    const bool name_ok = std::ranges::all_of(field.first(length), [](const std::byte b) {
        return is_platform_char(static_cast<char>(b));
    });
    const bool padding_ok = std::ranges::all_of(
        field.subspan(length), [](const std::byte b) { return b == std::byte{0}; });
    return (length > 0 && name_ok && padding_ok) ? length : 0;
}

// Có hai pak trùng file_hash không. Thứ tự sắp xếp chỉ để gom các hash bằng nhau lại cạnh nhau;
// phép so bằng vẫn đi qua equal_constant_time như mọi so sánh hash (X.9).
[[nodiscard]] bool has_duplicate_files(const std::span<const ManifestPak> paks) {
    std::vector<const crypto::Hash*> hashes;
    hashes.reserve(paks.size());
    for (const ManifestPak& pak : paks) {
        hashes.push_back(&pak.file_hash);
    }
    std::ranges::sort(hashes, [](const crypto::Hash* a, const crypto::Hash* b) {
        return std::ranges::lexicographical_compare(a->bytes, b->bytes);
    });
    for (usize i = 1; i < hashes.size(); ++i) {
        if (crypto::equal_constant_time(*hashes[i - 1], *hashes[i])) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ManifestPak read_pak_record(const std::span<const std::byte> record) noexcept {
    ManifestPak pak;
    pak.file_size = core::load_le<u64>(record.first<sizeof(u64)>());
    std::ranges::copy(record.subspan<kFileHashOffset, crypto::kHashSize>(),
                      pak.file_hash.bytes.begin());
    std::ranges::copy(record.subspan<kIndexHashOffset, crypto::kHashSize>(),
                      pak.index_hash.bytes.begin());
    return pak;
}

// Bước 1 của phần "Kiểm": cỡ, magic, version, flags, và pak_count khớp cỡ. Trả pak_count.
[[nodiscard]] Result<u32> check_structure(const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kManifestHeaderSize ||
        !std::ranges::equal(bytes.first<kManifestMagic.size()>(), kManifestMagic)) {
        return fail(ErrorCode::InvalidArgument, "manifest: không phải manifest");
    }
    const auto version = core::load_le<u16>(bytes.subspan<kManifestMagic.size(), sizeof(u16)>());
    if (version != kManifestVersion) {
        return fail(ErrorCode::Unimplemented, "manifest: phiên bản định dạng lạ", version);
    }
    const auto flags = core::load_le<u16>(bytes.subspan<kFlagsOffset, sizeof(u16)>());
    const auto pak_count = core::load_le<u32>(bytes.subspan<kPakCountOffset, sizeof(u32)>());
    if (flags != 0 || pak_count > kMaxManifestPaks ||
        bytes.size() != kManifestHeaderSize + (usize{pak_count} * kManifestPakRecordSize) +
                            crypto::kSignatureSize) {
        return fail(ErrorCode::DataLoss, "manifest: flags hay cỡ sai",
                    static_cast<i64>(bytes.size()));
    }
    return pak_count;
}

// Bước 2 và 3: người ký nằm trong `trusted_signers` (duyệt hết danh sách, không dừng sớm) và chữ
// ký đúng trên mọi byte đứng trước nó. Ghi người ký vào `signer`.
[[nodiscard]] Result<void> authenticate(const std::span<const std::byte> bytes,
                                        const std::span<const crypto::SigningPublicKey> trusted,
                                        crypto::SigningPublicKey& signer) noexcept {
    std::ranges::copy(bytes.subspan<kSignerOffset, crypto::kSigningPublicKeySize>(),
                      signer.bytes.begin());
    bool known = false;
    for (const crypto::SigningPublicKey& key : trusted) {
        known = crypto::equal_constant_time(key, signer) || known;
    }
    if (!known) {
        return fail(ErrorCode::Unauthenticated, "manifest: người ký không được tin");
    }
    const usize signed_size = bytes.size() - crypto::kSignatureSize;
    crypto::Signature signature;
    std::ranges::copy(bytes.subspan(signed_size), signature.bytes.begin());
    if (!crypto::verify(bytes.first(signed_size), signature, signer)) {
        return fail(ErrorCode::DataLoss, "manifest: chữ ký sai");
    }
    return {};
}

// Bước 4 cho các bản ghi pak.
[[nodiscard]] Result<void> read_paks(const std::span<const std::byte> bytes, const u32 pak_count,
                                     std::vector<ManifestPak>& paks) {
    paks.reserve(pak_count);
    for (usize i = 0; i < pak_count; ++i) {
        const ManifestPak pak = read_pak_record(bytes.subspan(
            kManifestHeaderSize + (i * kManifestPakRecordSize), kManifestPakRecordSize));
        if (pak.file_size < kPakHeaderSize) {
            return fail(ErrorCode::DataLoss, "manifest: pak nhỏ hơn header của pak",
                        static_cast<i64>(i));
        }
        paks.push_back(pak);
    }
    if (has_duplicate_files(paks)) {
        return fail(ErrorCode::DataLoss, "manifest: hai pak trùng hash");
    }
    return {};
}

}  // namespace

bool is_valid_platform(const std::string_view platform) noexcept {
    return !platform.empty() && platform.size() <= kManifestPlatformSize &&
           std::ranges::all_of(platform, is_platform_char);
}

std::string pak_file_name(const crypto::Hash& file_hash) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string name;
    name.reserve((file_hash.bytes.size() * 2) + 4);
    for (const std::byte b : file_hash.bytes) {
        const auto value = std::to_integer<u8>(b);
        name.push_back(kDigits[value >> 4U]);
        name.push_back(kDigits[value & 0x0FU]);
    }
    name += ".pak";
    return name;
}

Result<VerifiedManifest> VerifiedManifest::verify(
    const std::span<const std::byte> bytes,
    const std::span<const crypto::SigningPublicKey> trusted_signers,
    const std::string_view platform) noexcept {
    ORION_ASSERT(is_valid_platform(platform), "nền tảng sai luật: {}", platform);
    const Result<u32> pak_count = check_structure(bytes);
    if (!pak_count) {
        return std::unexpected(pak_count.error());
    }
    VerifiedManifest result;
    if (const Result<void> authentic = authenticate(bytes, trusted_signers, result.signer_);
        !authentic) {
        return std::unexpected(authentic.error());
    }

    // Từ đây dữ liệu đã được ký bởi một khoá tin cậy.
    const usize platform_size =
        platform_length(bytes.subspan<kPlatformOffset, kManifestPlatformSize>());
    if (platform_size == 0) {
        return fail(ErrorCode::DataLoss, "manifest: tên nền tảng sai luật");
    }
    const std::string_view tag = core::as_chars(bytes.subspan(kPlatformOffset, platform_size));
    if (tag != platform) {
        return fail(ErrorCode::FailedPrecondition, "manifest: của nền tảng khác");
    }
    std::ranges::copy(tag, result.platform_.begin());
    result.platform_size_ = platform_size;
    result.sequence_ = core::load_le<u64>(bytes.subspan<kSequenceOffset, sizeof(u64)>());
    if (const Result<void> read = read_paks(bytes, *pak_count, result.paks_); !read) {
        return std::unexpected(read.error());
    }
    return result;
}

#if ORION_DEV_TOOLS
Result<std::vector<std::byte>> build_manifest(const u64 sequence, const std::string_view platform,
                                              const std::span<const ManifestPak> paks,
                                              const crypto::SigningKeyPair& signer) {
    if (!is_valid_platform(platform) || paks.size() > kMaxManifestPaks) {
        return fail(ErrorCode::InvalidArgument, "manifest: nền tảng sai luật hay quá nhiều pak");
    }
    if (std::ranges::any_of(
            paks, [](const ManifestPak& pak) { return pak.file_size < kPakHeaderSize; }) ||
        has_duplicate_files(paks)) {
        return fail(ErrorCode::InvalidArgument, "manifest: pak nhỏ hơn header hay trùng hash");
    }
    const usize signed_size = kManifestHeaderSize + (paks.size() * kManifestPakRecordSize);
    std::vector<std::byte> manifest(signed_size + crypto::kSignatureSize);
    const std::span<std::byte> body = std::span(manifest).first(signed_size);
    core::ByteWriter writer(body);
    writer.write_bytes(kManifestMagic);
    writer.write<u16>(kManifestVersion);
    writer.write<u16>(0);
    writer.write<u32>(static_cast<u32>(paks.size()));
    writer.write<u64>(sequence);
    std::array<std::byte, kManifestPlatformSize> tag{};
    std::ranges::transform(platform, tag.begin(), [](const char c) { return std::byte(c); });
    writer.write_bytes(tag);
    writer.write_bytes(signer.public_key.view());
    for (const ManifestPak& pak : paks) {
        writer.write<u64>(pak.file_size);
        writer.write_bytes(pak.file_hash.view());
        writer.write_bytes(pak.index_hash.view());
    }
    ORION_VERIFY(!writer.overflowed() && writer.offset() == signed_size, "manifest: tính sai cỡ");
    const crypto::Signature signature = crypto::sign(body, signer.secret_key);
    std::ranges::copy(signature.bytes, std::span(manifest).subspan(signed_size).begin());
    return manifest;
}
#endif

}  // namespace orion::io
