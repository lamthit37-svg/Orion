#include "engine/io/vfs.hpp"

#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/io/detail/native_file.hpp"
#include "engine/io/file.hpp"
#include "engine/io/manifest.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/pak_builder.hpp"
#include "engine/io/path.hpp"
#include "engine/io/tests/support/dev_api.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "engine/jobs/thread.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

using testing::TempPath;

// Chiều DEV của ship_api_check.cpp: ở DEV các hàm này có, nên các khái niệm nhận ra được chúng.
static_assert(testing::MountsDirectories<Vfs>);
static_assert(testing::MountsUnlistedPaks<Vfs>);
static_assert(testing::OpensUnverified<PakReader>);
static_assert(testing::MountsManifests<Vfs>);

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

[[nodiscard]] VirtualPath vpath(const std::string_view text) {
    Result<VirtualPath> path = VirtualPath::parse(text);
    EXPECT_TRUE(path.has_value()) << text;
    return *path;
}

// Tên test viết thường: hợp lệ trong đường dẫn ảo, và khác nhau giữa các test chạy song song.
[[nodiscard]] std::string test_tag() {
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string tag = std::string(info->test_suite_name()) + "_" + info->name();
    std::ranges::transform(tag, tag.begin(), [](const char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return tag;
}

// Pak với các tệp `files`, cộng một entry mang tên test để hash (và tên tệp) khác giữa các test.
[[nodiscard]] PakBuilder builder_with(
    const std::vector<std::pair<std::string_view, std::string_view>>& files) {
    PakBuilder builder(3);
    for (const auto& [path, content] : files) {
        EXPECT_TRUE(builder.add(vpath(path), bytes_of(content)).has_value()) << path;
    }
    EXPECT_TRUE(builder.add(vpath("test.txt"), bytes_of(test_tag())).has_value());
    return builder;
}

// Một pak nằm trong thư mục tạm dưới tên theo hash như ở data/ của bản CHƠI; xoá khi ra khỏi phạm
// vi.
class PakOnDisk {
public:
    explicit PakOnDisk(const PakBuilder& builder) {
        const TempPath staging("staging.pak");
        const Result<PakSummary> summary = builder.write(staging.path());
        EXPECT_TRUE(summary.has_value());
        if (!summary) {
            return;
        }
        path_ = ::testing::TempDir() + pak_file_name(summary->file_hash);
        EXPECT_TRUE(detail::replace_file(staging.path().c_str(), path_.c_str()).has_value());
        pak_ = ManifestPak{summary->file_size, summary->file_hash, summary->index_hash};
    }
    PakOnDisk(const PakOnDisk&) = delete;
    PakOnDisk& operator=(const PakOnDisk&) = delete;
    PakOnDisk(PakOnDisk&&) = delete;
    PakOnDisk& operator=(PakOnDisk&&) = delete;
    ~PakOnDisk() {
        if (!path_.empty()) {
            static_cast<void>(detail::remove_file(path_.c_str()).has_value());
        }
    }

    [[nodiscard]] const ManifestPak& pak() const noexcept { return pak_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    ManifestPak pak_;
};

class VfsTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(crypto::initialize().has_value()); }

    // Manifest đã ký cho `paks` theo thứ tự.
    [[nodiscard]] Result<VerifiedManifest> manifest_for(
        const std::vector<ManifestPak>& paks) const {
        const Result<std::vector<std::byte>> bytes = build_manifest(1, "win64", paks, key_);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        const std::array trusted = {key_.public_key};
        return VerifiedManifest::verify(*bytes, trusted, "win64");
    }

    [[nodiscard]] static std::string read_text(const Vfs& vfs, const std::string_view path) {
        PakReadContext context;
        const Result<std::vector<std::byte>> content = vfs.read_all(vpath(path), context);
        EXPECT_TRUE(content.has_value()) << path;
        std::string text;
        for (const std::byte b : content.value_or(std::vector<std::byte>{})) {
            text.push_back(static_cast<char>(b));
        }
        return text;
    }

private:
    crypto::SigningKeyPair key_ = [] {
        EXPECT_TRUE(crypto::initialize().has_value());
        std::array<std::byte, crypto::kSigningSeedSize> seed{};
        seed.fill(std::byte{7});
        return crypto::signing_key_pair_from_seed(crypto::SigningSeed(seed));
    }();
};

TEST_F(VfsTest, MountsThePaksOfASignedManifest) {
    const PakOnDisk base(builder_with({{"a/one.txt", "một"}, {"b/two.txt", "hai"}}));
    const PakOnDisk extra(builder_with({{"c/three.txt", "ba"}}));
    const Result<VerifiedManifest> manifest = manifest_for({base.pak(), extra.pak()});
    ASSERT_TRUE(manifest.has_value());
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_manifest(*manifest, ::testing::TempDir()).has_value());
    EXPECT_EQ(vfs.mount_count(), 2U);
    EXPECT_EQ(read_text(vfs, "a/one.txt"), "một");
    EXPECT_EQ(read_text(vfs, "b/two.txt"), "hai");
    EXPECT_EQ(read_text(vfs, "c/three.txt"), "ba");
    const Result<u64> size = vfs.size(vpath("b/two.txt"));
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, std::string_view("hai").size());
}

TEST_F(VfsTest, LaterPakShadowsEarlierPak) {
    const PakOnDisk base(builder_with({{"data/items.bin", "gốc"}, {"data/zones.bin", "vùng"}}));
    const PakOnDisk patch(builder_with({{"data/items.bin", "đã vá"}}));
    const Result<VerifiedManifest> manifest = manifest_for({base.pak(), patch.pak()});
    ASSERT_TRUE(manifest.has_value());
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_manifest(*manifest, ::testing::TempDir()).has_value());
    EXPECT_EQ(read_text(vfs, "data/items.bin"), "đã vá");
    EXPECT_EQ(read_text(vfs, "data/zones.bin"), "vùng");
}

// Kiểm hash khi mount (X.9): pak thiếu, cỡ khác hay index_hash khác manifest thì không pak nào
// của lần mount đó được dùng.
TEST_F(VfsTest, MountChecksEveryPakAndMountsNothingOnFailure) {
    const PakOnDisk good(builder_with({{"a.txt", "a"}}));
    const PakOnDisk other(builder_with({{"b.txt", "b"}}));

    ManifestPak missing = other.pak();
    missing.file_hash.bytes[0] ^= std::byte{1};
    ManifestPak wrong_size = other.pak();
    wrong_size.file_size += 1;
    ManifestPak wrong_index = other.pak();
    wrong_index.index_hash.bytes[5] ^= std::byte{1};
    const std::array cases = {std::pair{missing, ErrorCode::NotFound},
                              std::pair{wrong_size, ErrorCode::DataLoss},
                              std::pair{wrong_index, ErrorCode::DataLoss}};
    for (const auto& [broken, expected] : cases) {
        const Result<VerifiedManifest> manifest = manifest_for({good.pak(), broken});
        ASSERT_TRUE(manifest.has_value());
        Vfs vfs;
        const Result<void> mounted = vfs.mount_manifest(*manifest, ::testing::TempDir());
        ASSERT_FALSE(mounted.has_value());
        EXPECT_EQ(mounted.error().code(), expected);
        EXPECT_EQ(vfs.mount_count(), 0U);
    }
}

TEST_F(VfsTest, LooseDirectoryIsReadFreshEachTime) {
    const std::string name = "orion_io_" + test_tag() + ".txt";
    const std::string path = ::testing::TempDir() + name;
    const auto write = [&](const std::string_view text) {
        Result<AtomicFileWriter> writer = AtomicFileWriter::create(path);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->write(bytes_of(text)).has_value());
        ASSERT_TRUE(writer->commit().has_value());
    };
    write("bản đầu");
    Vfs vfs;
    vfs.mount_directory(::testing::TempDir());
    EXPECT_EQ(read_text(vfs, name), "bản đầu");
    // Hot reload: tệp đổi trên đĩa thì lần đọc sau thấy ngay.
    write("bản sau, dài hơn");
    EXPECT_EQ(read_text(vfs, name), "bản sau, dài hơn");
    static_cast<void>(detail::remove_file(path.c_str()).has_value());
    const Result<u64> gone = vfs.size(vpath(name));
    ASSERT_FALSE(gone.has_value());
    EXPECT_EQ(gone.error().code(), ErrorCode::NotFound);
}

TEST_F(VfsTest, LooseDirectoryMountedLaterShadowsPak) {
    const std::string name = "orion_io_" + test_tag() + ".txt";
    const PakBuilder builder = builder_with({{name, "trong pak"}, {"only/in/pak.txt", "chỉ pak"}});
    const TempPath pak_path("data.pak");
    ASSERT_TRUE(builder.write(pak_path.path()).has_value());
    Result<File> pak_file = File::open(pak_path.path());
    ASSERT_TRUE(pak_file.has_value());
    Result<PakReader> reader = PakReader::open_unverified(std::move(*pak_file));
    ASSERT_TRUE(reader.has_value());

    const std::string loose = ::testing::TempDir() + name;
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(loose);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->write(bytes_of("tệp rời")).has_value());
    ASSERT_TRUE(writer->commit().has_value());

    Vfs vfs;
    vfs.mount_pak(std::move(*reader));
    vfs.mount_directory(::testing::TempDir());
    EXPECT_EQ(read_text(vfs, name), "tệp rời");
    EXPECT_EQ(read_text(vfs, "only/in/pak.txt"), "chỉ pak");
    static_cast<void>(detail::remove_file(loose.c_str()).has_value());
    EXPECT_EQ(read_text(vfs, name), "trong pak");
}

TEST_F(VfsTest, MissingFileAndSmallBuffer) {
    const Vfs empty;
    PakReadContext context;
    const Result<u64> nothing = empty.size(vpath("a.txt"));
    ASSERT_FALSE(nothing.has_value());
    EXPECT_EQ(nothing.error().code(), ErrorCode::NotFound);

    const PakOnDisk pak(builder_with({{"a.txt", "abcdef"}}));
    const Result<VerifiedManifest> manifest = manifest_for({pak.pak()});
    ASSERT_TRUE(manifest.has_value());
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_manifest(*manifest, ::testing::TempDir()).has_value());
    const Result<std::vector<std::byte>> missing = vfs.read_all(vpath("b.txt"), context);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    std::array<std::byte, 3> small{};
    const Result<usize> read = vfs.read(vpath("a.txt"), small, context);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::InvalidArgument);
    std::array<std::byte, 16> large{};
    const Result<usize> fits = vfs.read(vpath("a.txt"), large, context);
    ASSERT_TRUE(fits.has_value());
    EXPECT_EQ(*fits, 6U);
    const Result<usize> absent = vfs.read(vpath("b.txt"), large, context);
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().code(), ErrorCode::NotFound);
}

TEST_F(VfsTest, DirectoryGivenWithoutTrailingSeparator) {
    const std::string name = "orion_io_" + test_tag() + ".txt";
    const std::string path = ::testing::TempDir() + name;
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(path);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->write(bytes_of("rời")).has_value());
    ASSERT_TRUE(writer->commit().has_value());
    std::string directory = ::testing::TempDir();
    while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\')) {
        directory.pop_back();
    }
    Vfs vfs;
    vfs.mount_directory(directory);
    EXPECT_EQ(read_text(vfs, name), "rời");
    static_cast<void>(detail::remove_file(path.c_str()).has_value());
}

// Trong thư mục tệp rời, chỉ NotFound mới cho tìm tiếp ở mount trước; lỗi khác được trả ra, không
// bị che. Đường dẫn ảo ở đây trỏ tới chính thư mục tạm (tên viết thường: "tmp" trên Linux, "temp"
// trên Windows, nơi hệ thống tệp không phân biệt hoa thường): Linux báo InvalidArgument, Windows
// báo PermissionDenied.
TEST_F(VfsTest, LooseErrorsOtherThanNotFoundAreReported) {
    std::string temp = ::testing::TempDir();
    while (!temp.empty() && (temp.back() == '/' || temp.back() == '\\')) {
        temp.pop_back();
    }
    const usize separator = temp.find_last_of("/\\");
    ASSERT_NE(separator, std::string::npos) << temp;
    std::string child = temp.substr(separator + 1);
    std::ranges::transform(child, child.begin(), [](const char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    const Result<VirtualPath> directory_path = VirtualPath::parse(child);
    if (!directory_path) {
        GTEST_SKIP() << "tên thư mục tạm không là đường dẫn ảo hợp lệ: " << child;
    }
    const PakOnDisk pak(builder_with({{child, "trong pak"}}));
    Result<File> pak_file = File::open(pak.path());
    ASSERT_TRUE(pak_file.has_value());
    Result<PakReader> reader = PakReader::open_unverified(std::move(*pak_file));
    ASSERT_TRUE(reader.has_value());
    Vfs vfs;
    vfs.mount_pak(std::move(*reader));
    vfs.mount_directory(temp.substr(0, separator + 1));
    const Result<u64> size = vfs.size(*directory_path);
    ASSERT_FALSE(size.has_value());
    EXPECT_NE(size.error().code(), ErrorCode::NotFound);
}

TEST_F(VfsTest, CorruptPakEntryIsDataLossOnRead) {
    const PakBuilder builder = builder_with({{"a.txt", "nội dung sẽ bị sửa"}});
    Result<std::vector<std::byte>> image = builder.build();
    ASSERT_TRUE(image.has_value());
    // Blob đầu tiên bắt đầu ngay sau header; index vẫn nguyên nên pak vẫn mở được.
    (*image)[kPakHeaderSize] ^= std::byte{0x01};
    const TempPath path("corrupt.pak");
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(path.path());
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->write(*image).has_value());
    ASSERT_TRUE(writer->commit().has_value());
    Result<File> file = File::open(path.path());
    ASSERT_TRUE(file.has_value());
    Result<PakReader> reader = PakReader::open_unverified(std::move(*file));
    ASSERT_TRUE(reader.has_value());
    Vfs vfs;
    vfs.mount_pak(std::move(*reader));
    PakReadContext context;
    const Result<std::vector<std::byte>> content = vfs.read_all(vpath("a.txt"), context);
    ASSERT_FALSE(content.has_value());
    EXPECT_EQ(content.error().code(), ErrorCode::DataLoss);
}

// Nhiều luồng đọc chung một Vfs, mỗi luồng một context. Chạy dưới TSan ở linux-tsan.
TEST_F(VfsTest, ConcurrentReadersWithSeparateContexts) {
    const PakOnDisk base(builder_with(
        {{"a.txt", std::string_view("aaaa")}, {"b.txt", std::string_view("bbbbbbbb")}}));
    const PakOnDisk patch(builder_with({{"a.txt", std::string_view("AAAA-patched")}}));
    const Result<VerifiedManifest> manifest = manifest_for({base.pak(), patch.pak()});
    ASSERT_TRUE(manifest.has_value());
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_manifest(*manifest, ::testing::TempDir()).has_value());
    const Vfs& shared = vfs;
    std::atomic<u32> mismatches{0};
    std::vector<jobs::Thread> threads;
    for (u32 t = 0; t < 4; ++t) {
        Result<jobs::Thread> thread = jobs::Thread::start("vfs-reader", [&](jobs::StopToken) {
            PakReadContext context;
            for (u32 round = 0; round < 50; ++round) {
                const Result<std::vector<std::byte>> a = shared.read_all(vpath("a.txt"), context);
                const Result<std::vector<std::byte>> b = shared.read_all(vpath("b.txt"), context);
                const bool ok = a.has_value() && b.has_value() && *a == bytes_of("AAAA-patched") &&
                                *b == bytes_of("bbbbbbbb");
                mismatches.fetch_add(ok ? 0U : 1U);
            }
        });
        ASSERT_TRUE(thread.has_value());
        threads.push_back(std::move(*thread));
    }
    threads.clear();
    EXPECT_EQ(mismatches.load(), 0U);
}

// Đường của bản ship: đọc entry pak qua Vfs vào bộ đệm có sẵn không cấp phát (X.7).
TEST_F(VfsTest, PakReadsIntoAReservedContextDoNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    const std::string repetitive(50'000, 'r');
    const PakOnDisk pak(builder_with({{"big.txt", repetitive}, {"small.txt", "nhỏ"}}));
    const Result<VerifiedManifest> manifest = manifest_for({pak.pak()});
    ASSERT_TRUE(manifest.has_value());
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_manifest(*manifest, ::testing::TempDir()).has_value());
    const VirtualPath big = vpath("big.txt");
    const VirtualPath small = vpath("small.txt");
    PakReadContext context;
    std::vector<std::byte> out(repetitive.size());
    // Lần đầu dựng bộ giải nén của context.
    ASSERT_TRUE(vfs.read(big, out, context).has_value());
    const core::testing::AllocationScope scope;
    for (u32 round = 0; round < 10; ++round) {
        ASSERT_TRUE(vfs.read(big, out, context).has_value());
        ASSERT_TRUE(vfs.read(small, out, context).has_value());
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::io
