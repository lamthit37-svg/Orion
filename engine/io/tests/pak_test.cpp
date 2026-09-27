#include "engine/io/pak.hpp"

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/tests/support/alloc_hooks.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/io/file.hpp"
#include "engine/io/pak_builder.hpp"
#include "engine/io/path.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "engine/jobs/thread.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

using testing::TempPath;

class PakTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(crypto::initialize().has_value()); }
};

[[nodiscard]] VirtualPath vpath(const std::string_view text) {
    Result<VirtualPath> path = VirtualPath::parse(text);
    EXPECT_TRUE(path.has_value()) << text;
    return *path;
}

[[nodiscard]] std::vector<std::byte> text_bytes(const std::string_view text) {
    const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
    return {bytes.begin(), bytes.end()};
}

// Byte giả ngẫu nhiên, không nén được.
[[nodiscard]] std::vector<std::byte> noise(const usize size, u32 seed) {
    std::vector<std::byte> bytes(size);
    for (std::byte& b : bytes) {
        seed = (seed * 1'664'525U) + 1'013'904'223U;
        b = static_cast<std::byte>(seed >> 24U);
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> repetitive(const usize size) {
    std::vector<std::byte> bytes(size);
    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>("orion pak "[i % 10]);
    }
    return bytes;
}

// Mẫu nhỏ giống dữ liệu thiết kế đã biên dịch: nhiều trường lặp lại giữa các tệp.
[[nodiscard]] std::vector<std::byte> item_record(const u32 id) {
    const std::string rarity = id % 3 == 0 ? "rare" : "common";
    return text_bytes(R"({"id":)" + std::to_string(id) + R"(,"kind":"weapon","rarity":")" + rarity +
                      R"(","damage":)" + std::to_string((id * 7) % 50) + R"(,"name":"item_)" +
                      std::to_string(id) + R"("})");
}

// Chỉ số của entry mang đường dẫn này; báo lỗi test và trả 0 khi không có.
[[nodiscard]] usize find_entry(const PakIndex& index, const std::string_view path) {
    const std::optional<usize> found = index.find(path);
    EXPECT_TRUE(found.has_value()) << path;
    return found.value_or(0);
}

struct Contents {
    std::vector<std::pair<std::string, std::vector<std::byte>>> files;
};

[[nodiscard]] Contents sample_contents() {
    Contents contents;
    contents.files.emplace_back("a/empty.bin", std::vector<std::byte>{});
    contents.files.emplace_back("a/noise.bin", noise(3'000, 7));
    contents.files.emplace_back("b/repetitive.txt", repetitive(200'000));
    contents.files.emplace_back("b/small.txt", text_bytes("xin chào pak"));
    contents.files.emplace_back("c/deep/tree/leaf.dat", repetitive(64));
    return contents;
}

[[nodiscard]] PakBuilder builder_for(const Contents& contents) {
    PakBuilder builder(3);
    // Thêm ngược thứ tự để kiểm builder tự xếp theo đường dẫn.
    for (auto it = contents.files.rbegin(); it != contents.files.rend(); ++it) {
        EXPECT_TRUE(builder.add(vpath(it->first), it->second).has_value()) << it->first;
    }
    return builder;
}

[[nodiscard]] std::vector<std::byte> read_entry(const PakReader& reader,
                                                const std::string_view path,
                                                PakReadContext& context) {
    const std::optional<usize> index = reader.index().find(path);
    EXPECT_TRUE(index.has_value()) << path;
    if (!index) {
        return {};
    }
    std::vector<std::byte> out(static_cast<usize>(reader.index().entry(*index).original_size));
    const Result<usize> read = reader.read(*index, out, context);
    EXPECT_TRUE(read.has_value()) << path;
    return out;
}

TEST_F(PakTest, WrittenPakReadsBackEveryEntry) {
    const Contents contents = sample_contents();
    const TempPath temp("data.pak");
    const Result<PakSummary> summary = builder_for(contents).write(temp.path());
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->entry_count, contents.files.size());

    Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->size(), summary->file_size);
    std::vector<std::byte> whole(static_cast<usize>(file->size()));
    ASSERT_TRUE(file->read_exact(0, whole).has_value());
    EXPECT_TRUE(crypto::equal_constant_time(crypto::hash(whole), summary->file_hash));

    Result<PakReader> reader =
        PakReader::open(std::move(*file), summary->file_size, summary->index_hash);
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader->index().entry_count(), contents.files.size());
    PakReadContext context;
    for (const auto& [path, content] : contents.files) {
        EXPECT_EQ(read_entry(*reader, path, context), content) << path;
    }
    EXPECT_FALSE(reader->index().find("a/missing.bin").has_value());
    EXPECT_FALSE(reader->index().find("").has_value());
    EXPECT_FALSE(reader->index().find("zzz").has_value());
}

TEST_F(PakTest, EntriesAreSortedAndCompressedOnlyWhenSmaller) {
    const Result<std::vector<std::byte>> image = builder_for(sample_contents()).build();
    ASSERT_TRUE(image.has_value());
    const Result<PakIndex> index = PakIndex::parse_image(*image);
    ASSERT_TRUE(index.has_value());
    for (usize i = 1; i < index->entry_count(); ++i) {
        EXPECT_LT(index->path(i - 1), index->path(i));
    }
    const PakEntry& noisy = index->entry(find_entry(*index, "a/noise.bin"));
    EXPECT_EQ(noisy.compression, PakCompression::None);
    EXPECT_EQ(noisy.stored_size, noisy.original_size);
    const PakEntry& text = index->entry(find_entry(*index, "b/repetitive.txt"));
    EXPECT_EQ(text.compression, PakCompression::Zstd);
    EXPECT_LT(text.stored_size, text.original_size / 100);
    const PakEntry& empty = index->entry(find_entry(*index, "a/empty.bin"));
    EXPECT_EQ(empty.compression, PakCompression::None);
    EXPECT_EQ(empty.original_size, 0U);
    EXPECT_EQ(index->max_stored_size(), noisy.stored_size);
}

TEST_F(PakTest, SameInputsGiveIdenticalBytes) {
    const Result<std::vector<std::byte>> first = builder_for(sample_contents()).build();
    const Result<std::vector<std::byte>> second = builder_for(sample_contents()).build();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
}

TEST_F(PakTest, BuildAndWriteProduceTheSameFile) {
    const PakBuilder builder = builder_for(sample_contents());
    const Result<std::vector<std::byte>> image = builder.build();
    const TempPath temp("data.pak");
    const Result<PakSummary> summary = builder.write(temp.path());
    ASSERT_TRUE(image.has_value());
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(image->size(), summary->file_size);
    EXPECT_TRUE(crypto::equal_constant_time(crypto::hash(*image), summary->file_hash));
    const u64 index_offset = core::load_le<u64>(std::span(*image).subspan(24).first<8>());
    crypto::Hasher hasher;
    hasher.update(std::span(*image).first(kPakHeaderSize));
    hasher.update(std::span(*image).subspan(static_cast<usize>(index_offset)));
    EXPECT_TRUE(crypto::equal_constant_time(hasher.finish(), summary->index_hash));
}

TEST_F(PakTest, DictionaryCompressesSmallRecords) {
    std::vector<std::vector<std::byte>> samples;
    samples.reserve(2'000);
    for (u32 id = 0; id < 2'000; ++id) {
        samples.push_back(item_record(id));
    }
    std::vector<std::span<const std::byte>> views(samples.begin(), samples.end());
    const Result<std::vector<std::byte>> dictionary = train_pak_dictionary(views, 4'096);
    ASSERT_TRUE(dictionary.has_value());

    PakBuilder plain(3);
    PakBuilder with_dictionary(3);
    const Result<u8> id = with_dictionary.add_dictionary(*dictionary);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 1U);
    for (u32 record = 5'000; record < 5'050; ++record) {
        const VirtualPath path = vpath("items/" + std::to_string(record) + ".json");
        ASSERT_TRUE(plain.add(path, item_record(record)).has_value());
        ASSERT_TRUE(with_dictionary.add(path, item_record(record), *id).has_value());
    }
    const TempPath plain_path("plain.pak");
    const TempPath dictionary_path("dictionary.pak");
    const Result<PakSummary> plain_summary = plain.write(plain_path.path());
    const Result<PakSummary> dictionary_summary = with_dictionary.write(dictionary_path.path());
    ASSERT_TRUE(plain_summary.has_value());
    ASSERT_TRUE(dictionary_summary.has_value());

    Result<File> file = File::open(dictionary_path.path());
    ASSERT_TRUE(file.has_value());
    Result<PakReader> reader = PakReader::open(std::move(*file), dictionary_summary->file_size,
                                               dictionary_summary->index_hash);
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader->index().dictionary_count(), 1U);
    u64 plain_blobs = 0;
    u64 dictionary_blobs = 0;
    const Result<std::vector<std::byte>> plain_image = plain.build();
    ASSERT_TRUE(plain_image.has_value());
    const Result<PakIndex> plain_index = PakIndex::parse_image(*plain_image);
    ASSERT_TRUE(plain_index.has_value());
    PakReadContext context;
    for (usize i = 0; i < reader->index().entry_count(); ++i) {
        const PakEntry& entry = reader->index().entry(i);
        EXPECT_EQ(entry.dictionary, 1U);
        dictionary_blobs += entry.stored_size;
        plain_blobs += plain_index->entry(i).stored_size;
        const u32 record = 5'000 + static_cast<u32>(i);
        EXPECT_EQ(read_entry(*reader, reader->index().path(i), context), item_record(record));
    }
    // Mẫu nhỏ chỉ nén tốt khi có dictionary chung.
    EXPECT_LT(dictionary_blobs * 2, plain_blobs);
}

TEST_F(PakTest, WrongSizeOrIndexHashIsDataLoss) {
    const TempPath temp("data.pak");
    const Result<PakSummary> summary = builder_for(sample_contents()).write(temp.path());
    ASSERT_TRUE(summary.has_value());
    {
        Result<File> file = File::open(temp.path());
        ASSERT_TRUE(file.has_value());
        const Result<PakReader> reader =
            PakReader::open(std::move(*file), summary->file_size + 1, summary->index_hash);
        ASSERT_FALSE(reader.has_value());
        EXPECT_EQ(reader.error().code(), ErrorCode::DataLoss);
    }
    {
        Result<File> file = File::open(temp.path());
        ASSERT_TRUE(file.has_value());
        crypto::Hash wrong = summary->index_hash;
        wrong.bytes[0] ^= std::byte{1};
        const Result<PakReader> reader =
            PakReader::open(std::move(*file), summary->file_size, wrong);
        ASSERT_FALSE(reader.has_value());
        EXPECT_EQ(reader.error().code(), ErrorCode::DataLoss);
    }
}

// Sửa một byte trong blob sau khi pak đã được ký: mount vẫn qua (index nguyên vẹn) nhưng lần đọc
// entry đó báo DataLoss; entry khác vẫn đọc được.
TEST_F(PakTest, TamperedBlobIsCaughtWhenRead) {
    const Contents contents = sample_contents();
    const Result<std::vector<std::byte>> image = builder_for(contents).build();
    ASSERT_TRUE(image.has_value());
    const Result<PakIndex> index = PakIndex::parse_image(*image);
    ASSERT_TRUE(index.has_value());
    crypto::Hasher hasher;
    const u64 index_offset = core::load_le<u64>(std::span(*image).subspan(24).first<8>());
    hasher.update(std::span(*image).first(kPakHeaderSize));
    hasher.update(std::span(*image).subspan(static_cast<usize>(index_offset)));
    const crypto::Hash index_hash = hasher.finish();

    for (const std::string_view victim : {"b/repetitive.txt", "a/noise.bin"}) {
        SCOPED_TRACE(victim);
        std::vector<std::byte> tampered = *image;
        const PakEntry& entry = index->entry(find_entry(*index, victim));
        tampered[static_cast<usize>(entry.offset + (entry.stored_size / 2))] ^= std::byte{0x40};
        const TempPath temp("tampered.pak");
        Result<AtomicFileWriter> writer = AtomicFileWriter::create(temp.path());
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->write(tampered).has_value());
        ASSERT_TRUE(writer->commit().has_value());

        Result<File> file = File::open(temp.path());
        ASSERT_TRUE(file.has_value());
        Result<PakReader> reader = PakReader::open(std::move(*file), tampered.size(), index_hash);
        ASSERT_TRUE(reader.has_value());
        PakReadContext context;
        std::vector<std::byte> out(static_cast<usize>(entry.original_size));
        const Result<usize> read = reader->read(find_entry(reader->index(), victim), out, context);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error().code(), ErrorCode::DataLoss);
        EXPECT_EQ(read_entry(*reader, "b/small.txt", context), text_bytes("xin chào pak"));
    }
}

TEST_F(PakTest, ReadRejectsBadArguments) {
    const TempPath temp("data.pak");
    const Result<PakSummary> summary = builder_for(sample_contents()).write(temp.path());
    ASSERT_TRUE(summary.has_value());
    Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    Result<PakReader> reader =
        PakReader::open(std::move(*file), summary->file_size, summary->index_hash);
    ASSERT_TRUE(reader.has_value());
    PakReadContext context;
    std::array<std::byte, 4> small{};
    const Result<usize> out_of_range = reader->read(reader->index().entry_count(), small, context);
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().code(), ErrorCode::InvalidArgument);
    const Result<usize> too_small =
        reader->read(find_entry(reader->index(), "b/small.txt"), small, context);
    ASSERT_FALSE(too_small.has_value());
    EXPECT_EQ(too_small.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(PakTest, UnverifiedOpenForDevelopment) {
    const TempPath temp("data.pak");
    ASSERT_TRUE(builder_for(sample_contents()).write(temp.path()).has_value());
    Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    Result<PakReader> reader = PakReader::open_unverified(std::move(*file));
    ASSERT_TRUE(reader.has_value());
    PakReadContext context;
    EXPECT_EQ(read_entry(*reader, "b/small.txt", context), text_bytes("xin chào pak"));

    const TempPath tiny("tiny.pak");
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(tiny.path());
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->write(text_bytes("ORIONPAK")).has_value());
    ASSERT_TRUE(writer->commit().has_value());
    Result<File> tiny_file = File::open(tiny.path());
    ASSERT_TRUE(tiny_file.has_value());
    const Result<PakReader> rejected = PakReader::open_unverified(std::move(*tiny_file));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(PakTest, BuilderRejectsInvalidEntries) {
    PakBuilder builder(3);
    ASSERT_TRUE(builder.add(vpath("a.bin"), text_bytes("a")).has_value());
    const Result<void> duplicate = builder.add(vpath("a.bin"), text_bytes("b"));
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);
    const Result<void> no_dictionary = builder.add(vpath("b.bin"), text_bytes("b"), 1);
    ASSERT_FALSE(no_dictionary.has_value());
    EXPECT_EQ(no_dictionary.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(PakTest, BuilderRejectsInvalidDictionaries) {
    PakBuilder builder(3);
    const Result<u8> empty = builder.add_dictionary({});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);
    // Có magic dictionary của Zstd (0xEC30A437) mà bảng entropy là rác.
    std::vector<std::byte> corrupt = noise(512, 3);
    const std::array<std::byte, 4> magic{std::byte{0x37}, std::byte{0xA4}, std::byte{0x30},
                                         std::byte{0xEC}};
    std::ranges::copy(magic, corrupt.begin());
    const Result<u8> rejected = builder.add_dictionary(corrupt);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::DataLoss);

    const std::vector<std::byte> raw_content = repetitive(1'024);
    for (u32 k = 1; k <= kMaxPakDictionaries; ++k) {
        const Result<u8> id = builder.add_dictionary(raw_content);
        ASSERT_TRUE(id.has_value());
        EXPECT_EQ(*id, k);
    }
    const Result<u8> too_many = builder.add_dictionary(raw_content);
    ASSERT_FALSE(too_many.has_value());
    EXPECT_EQ(too_many.error().code(), ErrorCode::ResourceExhausted);

    const std::vector<std::span<const std::byte>> none;
    const Result<std::vector<std::byte>> untrained = train_pak_dictionary(none, 4'096);
    ASSERT_FALSE(untrained.has_value());
    EXPECT_EQ(untrained.error().code(), ErrorCode::InvalidArgument);
}

// Đọc mọi entry `rounds` lần bằng một context riêng; trả số lần đọc hỏng hay ra nội dung sai.
[[nodiscard]] u32 count_bad_reads(const PakReader& reader, const Contents& contents,
                                  const u32 rounds) {
    PakReadContext context;
    u32 bad = 0;
    for (u32 round = 0; round < rounds; ++round) {
        for (const auto& [path, content] : contents.files) {
            std::vector<std::byte> out(content.size());
            const Result<usize> read =
                reader.read(reader.index().find(path).value_or(0), out, context);
            bad += (!read.has_value() || out != content) ? 1U : 0U;
        }
    }
    return bad;
}

// Nhiều luồng đọc chung một PakReader, mỗi luồng một context; mọi entry xin dictionary nên các
// luồng dùng chung một ZSTD_DDict. TSan ở linux-tsan thấy phần code của dự án; bên trong zstd không
// được instrument, phần đó đo ở NGHI-NGO-033.
TEST_F(PakTest, ConcurrentReadersWithSeparateContexts) {
    const Contents contents = sample_contents();
    PakBuilder builder(3);
    const Result<u8> dictionary = builder.add_dictionary(repetitive(4'096));
    ASSERT_TRUE(dictionary.has_value());
    for (const auto& [path, content] : contents.files) {
        ASSERT_TRUE(builder.add(vpath(path), content, *dictionary).has_value()) << path;
    }
    const TempPath temp("data.pak");
    const Result<PakSummary> summary = builder.write(temp.path());
    ASSERT_TRUE(summary.has_value());
    Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    Result<PakReader> opened =
        PakReader::open(std::move(*file), summary->file_size, summary->index_hash);
    ASSERT_TRUE(opened.has_value());
    const PakReader& reader = *opened;
    usize with_dictionary = 0;
    for (usize i = 0; i < reader.index().entry_count(); ++i) {
        with_dictionary += reader.index().entry(i).dictionary != 0 ? 1U : 0U;
    }
    ASSERT_GE(with_dictionary, 2U);
    std::atomic<u32> mismatches{0};
    std::vector<jobs::Thread> threads;
    for (u32 t = 0; t < 4; ++t) {
        Result<jobs::Thread> thread = jobs::Thread::start("pak-reader", [&](jobs::StopToken) {
            mismatches.fetch_add(count_bad_reads(reader, contents, 20));
        });
        ASSERT_TRUE(thread.has_value());
        threads.push_back(std::move(*thread));
    }
    threads.clear();
    EXPECT_EQ(mismatches.load(), 0U);
}

TEST_F(PakTest, ReadsAfterReserveDoNotAllocate) {
    if (!core::testing::allocation_counting_enabled()) {
        GTEST_SKIP() << "TSan giữ operator new cho runtime của nó; không đếm được";
    }
    const Contents contents = sample_contents();
    const TempPath temp("data.pak");
    const Result<PakSummary> summary = builder_for(contents).write(temp.path());
    ASSERT_TRUE(summary.has_value());
    Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    Result<PakReader> reader =
        PakReader::open(std::move(*file), summary->file_size, summary->index_hash);
    ASSERT_TRUE(reader.has_value());
    PakReadContext context;
    context.reserve(reader->index().max_stored_size());
    std::vector<std::byte> out(200'000);
    // Lần đầu tạo bộ giải nén của context; sau đó không cấp phát nữa.
    ASSERT_TRUE(
        reader->read(find_entry(reader->index(), "b/repetitive.txt"), out, context).has_value());
    const core::testing::AllocationScope scope;
    for (usize i = 0; i < reader->index().entry_count(); ++i) {
        ASSERT_TRUE(reader->read(i, out, context).has_value());
    }
    EXPECT_EQ(scope.count(), 0U);
}

}  // namespace
}  // namespace orion::io
