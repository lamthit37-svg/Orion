// Manifest đã ký (docs/formats/manifest.md). `craft` ghi byte theo đặc tả, độc lập với
// build_manifest, rồi ký bằng engine/crypto; mỗi luật của bảng "Lỗi" được kiểm bằng cách sửa đúng
// một trường rồi ký lại, để lỗi đo được là của luật đó chứ không phải của chữ ký.

#include "engine/io/manifest.hpp"

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/io/pak.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace orion::io {
namespace {

[[nodiscard]] crypto::SigningKeyPair key_from(const u8 fill) {
    EXPECT_TRUE(crypto::initialize().has_value());
    std::array<std::byte, crypto::kSigningSeedSize> seed{};
    seed.fill(std::byte{fill});
    return crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
}

[[nodiscard]] ManifestPak numbered_pak(const u32 n) {
    ManifestPak pak;
    pak.file_size = 1'000 + n;
    core::store_le<u32>(std::span(pak.file_hash.bytes).first<4>(), n);
    pak.index_hash.bytes.fill(std::byte{0xA5});
    core::store_le<u32>(std::span(pak.index_hash.bytes).first<4>(), n);
    return pak;
}

// Các trường của một manifest, ghi thẳng theo bảng của đặc tả.
struct Fields {
    std::array<std::byte, 8> magic = kManifestMagic;
    u16 version = 1;
    u16 flags = 0;
    std::optional<u32> declared_count;  // Mặc định: số bản ghi thật.
    u64 sequence = 7;
    std::array<std::byte, 8> platform{std::byte{'w'}, std::byte{'i'}, std::byte{'n'},
                                      std::byte{'6'}, std::byte{'4'}};
    std::vector<ManifestPak> paks;
};

[[nodiscard]] std::vector<std::byte> craft(const Fields& fields,
                                           const crypto::SigningKeyPair& key) {
    const usize count = fields.paks.size();
    std::vector<std::byte> out(64 + (count * 72) + 64);
    const std::span<std::byte> bytes(out);
    std::ranges::copy(fields.magic, bytes.begin());
    core::store_le<u16>(bytes.subspan<8, 2>(), fields.version);
    core::store_le<u16>(bytes.subspan<10, 2>(), fields.flags);
    core::store_le<u32>(bytes.subspan<12, 4>(),
                        fields.declared_count.value_or(static_cast<u32>(count)));
    core::store_le<u64>(bytes.subspan<16, 8>(), fields.sequence);
    std::ranges::copy(fields.platform, bytes.begin() + 24);
    std::ranges::copy(key.public_key.bytes, bytes.begin() + 32);
    for (usize i = 0; i < count; ++i) {
        const std::span<std::byte> record = bytes.subspan(64 + (i * 72), 72);
        core::store_le<u64>(record.first<8>(), fields.paks[i].file_size);
        std::ranges::copy(fields.paks[i].file_hash.bytes, record.begin() + 8);
        std::ranges::copy(fields.paks[i].index_hash.bytes, record.begin() + 40);
    }
    const usize signed_size = out.size() - 64;
    const crypto::Signature signature = crypto::sign(bytes.first(signed_size), key.secret_key);
    std::ranges::copy(signature.bytes, bytes.begin() + static_cast<std::ptrdiff_t>(signed_size));
    return out;
}

class ManifestTest : public ::testing::Test {
protected:
    [[nodiscard]] const crypto::SigningKeyPair& release() const noexcept { return release_; }
    [[nodiscard]] const crypto::SigningKeyPair& other() const noexcept { return other_; }

    [[nodiscard]] Result<VerifiedManifest> verify(const std::span<const std::byte> bytes,
                                                  const std::string_view platform = "win64") const {
        const std::array trusted = {release().public_key};
        return VerifiedManifest::verify(bytes, trusted, platform);
    }

    // Mã lỗi khi manifest dựng từ `fields` bị từ chối; nullopt khi nó được nhận.
    [[nodiscard]] std::optional<ErrorCode> rejection(const Fields& fields) const {
        const Result<VerifiedManifest> manifest = verify(craft(fields, release()));
        if (manifest) {
            return std::nullopt;
        }
        return manifest.error().code();
    }

private:
    crypto::SigningKeyPair release_ = key_from(1);
    crypto::SigningKeyPair other_ = key_from(2);
};

TEST_F(ManifestTest, RoundTripKeepsEveryField) {
    const std::array paks = {numbered_pak(1), numbered_pak(2), numbered_pak(3)};
    const Result<std::vector<std::byte>> bytes = build_manifest(42, "win64", paks, release());
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->size(), 64U + (3U * 72U) + 64U);
    const Result<VerifiedManifest> manifest = verify(*bytes);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->sequence(), 42U);
    EXPECT_EQ(manifest->platform(), "win64");
    EXPECT_TRUE(crypto::equal_constant_time(manifest->signer(), release().public_key));
    ASSERT_EQ(manifest->paks().size(), paks.size());
    for (usize i = 0; i < paks.size(); ++i) {
        EXPECT_EQ(manifest->paks()[i].file_size, paks[i].file_size);
        EXPECT_TRUE(crypto::equal_constant_time(manifest->paks()[i].file_hash, paks[i].file_hash));
        EXPECT_TRUE(
            crypto::equal_constant_time(manifest->paks()[i].index_hash, paks[i].index_hash));
    }
}

TEST_F(ManifestTest, BuilderMatchesIndependentWriter) {
    Fields fields;
    fields.sequence = 99;
    fields.paks = {numbered_pak(5), numbered_pak(6)};
    const Result<std::vector<std::byte>> built =
        build_manifest(fields.sequence, "win64", fields.paks, release());
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(*built, craft(fields, release()));
}

TEST_F(ManifestTest, EmptyManifestIsValidAndBytesAreDeterministic) {
    const Result<std::vector<std::byte>> first = build_manifest(1, "android", {}, release());
    const Result<std::vector<std::byte>> second = build_manifest(1, "android", {}, release());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    const Result<VerifiedManifest> manifest = verify(*first, "android");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(manifest->paks().empty());
}

TEST_F(ManifestTest, OnlyTrustedSignersAreAccepted) {
    const Result<std::vector<std::byte>> bytes = build_manifest(3, "win64", {}, release());
    ASSERT_TRUE(bytes.has_value());
    // Xoay khoá: danh sách có cả khoá cũ và khoá mới.
    const std::array both = {other().public_key, release().public_key};
    EXPECT_TRUE(VerifiedManifest::verify(*bytes, both, "win64").has_value());
    const std::array only_other = {other().public_key};
    const Result<VerifiedManifest> untrusted =
        VerifiedManifest::verify(*bytes, only_other, "win64");
    ASSERT_FALSE(untrusted.has_value());
    EXPECT_EQ(untrusted.error().code(), ErrorCode::Unauthenticated);
    const Result<VerifiedManifest> none = VerifiedManifest::verify(*bytes, {}, "win64");
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::Unauthenticated);
}

TEST_F(ManifestTest, ManifestOfAnotherPlatformIsFailedPrecondition) {
    const Result<std::vector<std::byte>> bytes = build_manifest(3, "android", {}, release());
    ASSERT_TRUE(bytes.has_value());
    const Result<VerifiedManifest> manifest = verify(*bytes, "win64");
    ASSERT_FALSE(manifest.has_value());
    EXPECT_EQ(manifest.error().code(), ErrorCode::FailedPrecondition);
}

// Lật một bit ở từng byte của tệp: không byte nào đổi được mà manifest vẫn được nhận, và mỗi vùng
// cho đúng mã lỗi của thứ tự kiểm trong đặc tả.
TEST_F(ManifestTest, EveryByteIsProtected) {
    const std::array paks = {numbered_pak(1), numbered_pak(2)};
    const Result<std::vector<std::byte>> original = build_manifest(8, "win64", paks, release());
    ASSERT_TRUE(original.has_value());
    for (usize i = 0; i < original->size(); ++i) {
        std::vector<std::byte> bytes = *original;
        bytes[i] ^= std::byte{0x01};
        const Result<VerifiedManifest> manifest = verify(bytes);
        ASSERT_FALSE(manifest.has_value()) << "byte " << i;
        ErrorCode expected = ErrorCode::DataLoss;
        if (i < 8) {
            expected = ErrorCode::InvalidArgument;
        } else if (i < 10) {
            expected = ErrorCode::Unimplemented;
        } else if (i >= 32 && i < 64) {
            expected = ErrorCode::Unauthenticated;
        }
        EXPECT_EQ(manifest.error().code(), expected) << "byte " << i;
    }
}

TEST_F(ManifestTest, HeaderRules) {
    Fields fields;
    fields.paks = {numbered_pak(1)};
    EXPECT_TRUE(verify(craft(fields, release())).has_value());
    Fields wrong = fields;
    wrong.magic[7] = std::byte{'X'};
    EXPECT_EQ(rejection(wrong), ErrorCode::InvalidArgument);
    wrong = fields;
    wrong.version = 2;
    EXPECT_EQ(rejection(wrong), ErrorCode::Unimplemented);
    wrong = fields;
    wrong.flags = 1;
    EXPECT_EQ(rejection(wrong), ErrorCode::DataLoss);
    wrong = fields;
    wrong.declared_count = 2;
    EXPECT_EQ(rejection(wrong), ErrorCode::DataLoss);
    wrong = fields;
    wrong.declared_count = 0;
    EXPECT_EQ(rejection(wrong), ErrorCode::DataLoss);
}

TEST_F(ManifestTest, PlatformRules) {
    const auto with_platform = [](const std::string_view name) {
        Fields fields;
        fields.platform = {};
        std::ranges::transform(name, fields.platform.begin(),
                               [](const char c) { return std::byte(c); });
        return fields;
    };
    for (const std::string_view bad : {std::string_view(""), std::string_view("Win64"),
                                       std::string_view("win-64"), std::string_view("win 64")}) {
        EXPECT_EQ(rejection(with_platform(bad)), ErrorCode::DataLoss) << bad;
    }
    // Sau byte 0 đầu tiên phải toàn 0.
    Fields gap = with_platform("win64");
    gap.platform[6] = std::byte{'x'};
    EXPECT_EQ(rejection(gap), ErrorCode::DataLoss);
    // Đủ 8 ký tự, không có byte 0 nào, là hợp lệ.
    EXPECT_TRUE(verify(craft(with_platform("abcd1234"), release()), "abcd1234").has_value());
}

TEST_F(ManifestTest, PakRecordRules) {
    Fields fields;
    fields.paks = {numbered_pak(1), numbered_pak(2)};
    fields.paks[1].file_size = kPakHeaderSize;
    EXPECT_TRUE(verify(craft(fields, release())).has_value());
    fields.paks[1].file_size = kPakHeaderSize - 1;
    EXPECT_EQ(rejection(fields), ErrorCode::DataLoss);
    fields.paks[1] = numbered_pak(1);
    fields.paks[1].index_hash.bytes.fill(std::byte{0});
    EXPECT_EQ(rejection(fields), ErrorCode::DataLoss);
}

TEST_F(ManifestTest, PakCountLimit) {
    std::vector<ManifestPak> paks;
    paks.reserve(kMaxManifestPaks + 1U);
    for (u32 i = 0; i < kMaxManifestPaks; ++i) {
        paks.push_back(numbered_pak(i));
    }
    const Result<std::vector<std::byte>> full = build_manifest(1, "win64", paks, release());
    ASSERT_TRUE(full.has_value());
    const Result<VerifiedManifest> manifest = verify(*full);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->paks().size(), kMaxManifestPaks);
    paks.push_back(numbered_pak(kMaxManifestPaks));
    Fields fields;
    fields.paks = paks;
    EXPECT_EQ(rejection(fields), ErrorCode::DataLoss);
}

TEST_F(ManifestTest, TooShortIsNotAManifest) {
    const Result<std::vector<std::byte>> bytes = build_manifest(1, "win64", {}, release());
    ASSERT_TRUE(bytes.has_value());
    const std::span<const std::byte> all(*bytes);
    const Result<VerifiedManifest> header_only = verify(all.first(kManifestHeaderSize - 1));
    ASSERT_FALSE(header_only.has_value());
    EXPECT_EQ(header_only.error().code(), ErrorCode::InvalidArgument);
    const Result<VerifiedManifest> no_signature = verify(all.first(kManifestHeaderSize));
    ASSERT_FALSE(no_signature.has_value());
    EXPECT_EQ(no_signature.error().code(), ErrorCode::DataLoss);
    const Result<VerifiedManifest> empty = verify({});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ManifestTest, BuilderRejectsInvalidInput) {
    for (const std::string_view platform :
         {std::string_view(""), std::string_view("Win64"), std::string_view("win64abcd")}) {
        const Result<std::vector<std::byte>> bytes = build_manifest(1, platform, {}, release());
        ASSERT_FALSE(bytes.has_value()) << platform;
        EXPECT_EQ(bytes.error().code(), ErrorCode::InvalidArgument);
    }
    std::array paks = {numbered_pak(1), numbered_pak(1)};
    const Result<std::vector<std::byte>> duplicate = build_manifest(1, "win64", paks, release());
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);
    paks[1] = numbered_pak(2);
    paks[1].file_size = 10;
    const Result<std::vector<std::byte>> small = build_manifest(1, "win64", paks, release());
    ASSERT_FALSE(small.has_value());
    EXPECT_EQ(small.error().code(), ErrorCode::InvalidArgument);
    const std::vector<ManifestPak> too_many(kMaxManifestPaks + 1U, numbered_pak(3));
    const Result<std::vector<std::byte>> overflow = build_manifest(1, "win64", too_many, release());
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::InvalidArgument);
}

TEST(ManifestNames, PlatformNamesFollowTheRule) {
    for (const std::string_view good : {"win64", "android", "ios", "linux", "a", "12345678"}) {
        EXPECT_TRUE(is_valid_platform(good)) << good;
    }
    for (const std::string_view bad : {"", "123456789", "Win64", "win_64", "win64\n"}) {
        EXPECT_FALSE(is_valid_platform(bad)) << bad;
    }
}

TEST(ManifestNames, PakFileNameIsLowercaseHexOfTheHash) {
    crypto::Hash hash;
    for (usize i = 0; i < hash.bytes.size(); ++i) {
        hash.bytes[i] = static_cast<std::byte>((i % 16U) * 0x11U);
    }
    EXPECT_EQ(pak_file_name(hash),
              "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff.pak");
}

}  // namespace
}  // namespace orion::io
