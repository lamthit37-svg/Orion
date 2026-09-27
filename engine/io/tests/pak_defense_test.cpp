// Lớp phòng thủ thứ hai của PakReader (ADR 0012): blob đã khớp hash trong index vẫn bị kiểm frame
// Zstd trước khi giải nén. Pak ở đây được dựng tay theo docs/formats/pak.md, độc lập với
// PakBuilder, và hash của entry được tính cho khớp blob, nên chỉ phép kiểm frame mới bắt được lỗi.

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/io/file.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/pak_builder.hpp"
#include "engine/io/path.hpp"
#include "engine/io/tests/support/temp_file.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

using testing::TempPath;

struct CraftedEntry {
    std::string path;
    std::vector<std::byte> blob;
    u64 original_size = 0;
    u8 compression = 0;
    u8 dictionary = 0;
};

// Dựng pak đúng theo bảng của định dạng: header, blob liền nhau từ byte 64, rồi index.
[[nodiscard]] std::vector<std::byte> craft_pak(
    const std::vector<CraftedEntry>& entries,
    const std::vector<std::vector<std::byte>>& dictionaries = {}) {
    std::vector<std::byte> blobs;
    std::vector<std::byte> index;
    std::string strings;
    std::vector<std::byte> dictionary_data;
    for (const CraftedEntry& entry : entries) {
        std::array<std::byte, kPakEntrySize> record{};
        core::ByteWriter writer(record);
        writer.write<u64>(kPakHeaderSize + blobs.size());
        writer.write<u64>(entry.blob.size());
        writer.write<u64>(entry.original_size);
        writer.write<u32>(static_cast<u32>(strings.size()));
        writer.write<u16>(static_cast<u16>(entry.path.size()));
        writer.write<u8>(entry.compression);
        writer.write<u8>(entry.dictionary);
        writer.write_bytes(crypto::hash(entry.blob).bytes);
        index.insert(index.end(), record.begin(), record.end());
        blobs.insert(blobs.end(), entry.blob.begin(), entry.blob.end());
        strings += entry.path;
    }
    for (const std::vector<std::byte>& dictionary : dictionaries) {
        std::array<std::byte, kPakDictionaryRecordSize> record{};
        core::ByteWriter writer(record);
        writer.write<u32>(static_cast<u32>(dictionary_data.size()));
        writer.write<u32>(static_cast<u32>(dictionary.size()));
        index.insert(index.end(), record.begin(), record.end());
        dictionary_data.insert(dictionary_data.end(), dictionary.begin(), dictionary.end());
    }
    const std::span<const std::byte> string_bytes = std::as_bytes(std::span(strings));
    index.insert(index.end(), string_bytes.begin(), string_bytes.end());
    index.insert(index.end(), dictionary_data.begin(), dictionary_data.end());

    std::vector<std::byte> pak(kPakHeaderSize);
    core::ByteWriter header(pak);
    header.write_bytes(kPakMagic);
    header.write<u16>(kPakVersion);
    header.write<u16>(0);
    header.write<u32>(static_cast<u32>(entries.size()));
    header.write<u32>(static_cast<u32>(dictionaries.size()));
    header.write<u32>(static_cast<u32>(strings.size()));
    header.write<u64>(kPakHeaderSize + blobs.size());
    header.write<u64>(index.size());
    pak.insert(pak.end(), blobs.begin(), blobs.end());
    pak.insert(pak.end(), index.begin(), index.end());
    return pak;
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::initializer_list<u8> values) {
    std::vector<std::byte> bytes;
    for (const u8 value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

class PakDefenseTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(crypto::initialize().has_value()); }

    // Ghi pak ra tệp tạm, mở không có manifest và đọc entry 0.
    [[nodiscard]] static Result<usize> read_first(const std::vector<std::byte>& pak) {
        const TempPath temp("crafted.pak");
        Result<AtomicFileWriter> writer = AtomicFileWriter::create(temp.path());
        EXPECT_TRUE(writer.has_value());
        EXPECT_TRUE(writer->write(pak).has_value());
        EXPECT_TRUE(writer->commit().has_value());
        Result<File> file = File::open(temp.path());
        EXPECT_TRUE(file.has_value());
        Result<PakReader> reader = PakReader::open_unverified(std::move(*file));
        if (!reader) {
            return std::unexpected(reader.error());
        }
        PakReadContext context;
        std::vector<std::byte> out(static_cast<usize>(reader->index().entry(0).original_size));
        return reader->read(0, out, context);
    }
};

// Khung frame Zstd có một block thô: magic, Frame_Header_Descriptor 0x20 (một segment, content
// size 1 byte), content size, block header (block cuối, kiểu raw, cỡ n), rồi n byte.
[[nodiscard]] std::vector<std::byte> raw_block_frame(const u8 declared_size,
                                                     const std::vector<std::byte>& payload) {
    std::vector<std::byte> frame = bytes_of({0x28, 0xB5, 0x2F, 0xFD, 0x20, declared_size});
    const u32 block_header = 1U | (static_cast<u32>(payload.size()) << 3U);
    frame.push_back(static_cast<std::byte>(block_header & 0xFFU));
    frame.push_back(static_cast<std::byte>((block_header >> 8U) & 0xFFU));
    frame.push_back(static_cast<std::byte>((block_header >> 16U) & 0xFFU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

TEST_F(PakDefenseTest, HandCraftedPakFollowsTheSpecification) {
    const std::vector<std::byte> payload = bytes_of({'o', 'r', 'i', 'o', 'n'});
    // Frame dựng tay đúng luật: PakReader đọc được, nên cách dựng ở trên khớp định dạng.
    const Result<usize> read =
        read_first(craft_pak({{"a.bin", raw_block_frame(5, payload), 5, 1, 0}}));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 5U);
    const Result<usize> stored = read_first(craft_pak({{"a.bin", payload, 5, 0, 0}}));
    ASSERT_TRUE(stored.has_value());
}

TEST_F(PakDefenseTest, NonCurrentFramesAreRejected) {
    const std::vector<std::byte> payload(16, std::byte{'x'});
    for (const u32 magic : {0xFD2F'B527U, 0x184D'2A50U, 0x0000'0000U}) {
        SCOPED_TRACE(magic);
        std::vector<std::byte> frame = raw_block_frame(16, payload);
        core::store_le<u32>(std::span(frame).first<4>(), magic);
        const Result<usize> read = read_first(craft_pak({{"a.bin", frame, 16, 1, 0}}));
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error().code(), ErrorCode::DataLoss);
    }
}

TEST_F(PakDefenseTest, FrameMustDeclareTheOriginalSizeAndStandAlone) {
    const std::vector<std::byte> payload(8, std::byte{'y'});
    // Content size trong frame khác original_size của entry.
    const Result<usize> mismatch =
        read_first(craft_pak({{"a.bin", raw_block_frame(8, payload), 9, 1, 0}}));
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(mismatch.error().code(), ErrorCode::DataLoss);
    // Byte thừa sau frame.
    std::vector<std::byte> trailing = raw_block_frame(8, payload);
    trailing.push_back(std::byte{0});
    const Result<usize> extra = read_first(craft_pak({{"a.bin", trailing, 8, 1, 0}}));
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code(), ErrorCode::DataLoss);
    // Frame chỉ có 3 byte.
    const Result<usize> tiny =
        read_first(craft_pak({{"a.bin", bytes_of({0x28, 0xB5, 0x2F}), 1, 1, 0}}));
    ASSERT_FALSE(tiny.has_value());
    EXPECT_EQ(tiny.error().code(), ErrorCode::DataLoss);
}

// Frame khai content size 10 mà block chỉ cho 5 byte: cấu trúc frame đúng, giải nén thì hỏng.
TEST_F(PakDefenseTest, DecompressionFailureIsDataLoss) {
    const std::vector<std::byte> payload(5, std::byte{'z'});
    const Result<usize> read =
        read_first(craft_pak({{"a.bin", raw_block_frame(10, payload), 10, 1, 0}}));
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::DataLoss);
}

TEST_F(PakDefenseTest, CorruptDictionaryIsDataLoss) {
    // Magic dictionary của Zstd (0xEC30A437) theo sau là rác: ZSTD_createDDict trả NULL.
    std::vector<std::byte> dictionary = bytes_of({0x37, 0xA4, 0x30, 0xEC, 1, 2, 3, 4});
    dictionary.resize(300, std::byte{0x55});
    const Result<usize> read =
        read_first(craft_pak({{"a.bin", bytes_of({'a'}), 1, 0, 0}}, {dictionary}));
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::DataLoss);
}

TEST(PakBuilderErrors, TooFewSamplesCannotTrainADictionary) {
    const std::vector<std::byte> sample(8, std::byte{'s'});
    const std::vector<std::span<const std::byte>> samples(3, std::span<const std::byte>(sample));
    const Result<std::vector<std::byte>> dictionary = train_pak_dictionary(samples, 4'096);
    ASSERT_FALSE(dictionary.has_value());
    EXPECT_EQ(dictionary.error().code(), ErrorCode::DataLoss);
}

TEST(PakBuilderErrors, WriteIntoMissingDirectoryIsNotFound) {
    ASSERT_TRUE(crypto::initialize().has_value());
    const PakBuilder builder(1);
    const Result<PakSummary> summary =
        builder.write(::testing::TempDir() + "orion_io_no_such_dir/data.pak");
    ASSERT_FALSE(summary.has_value());
    EXPECT_EQ(summary.error().code(), ErrorCode::NotFound);
}

TEST(PakBuilderErrors, MovedBuilderKeepsItsEntries) {
    ASSERT_TRUE(crypto::initialize().has_value());
    PakBuilder first(1);
    const Result<VirtualPath> path = VirtualPath::parse("a.bin");
    ASSERT_TRUE(path.has_value());
    ASSERT_TRUE(first.add(*path, bytes_of({'a'})).has_value());
    PakBuilder second = std::move(first);
    PakBuilder third(1);
    third = std::move(second);
    const Result<std::vector<std::byte>> image = third.build();
    ASSERT_TRUE(image.has_value());
    const Result<PakIndex> index = PakIndex::parse_image(*image);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index->entry_count(), 1U);
}

TEST(PakReaderMoves, ReaderAndContextMoveWithTheirState) {
    ASSERT_TRUE(crypto::initialize().has_value());
    PakBuilder builder(1);
    const Result<VirtualPath> path = VirtualPath::parse("a.bin");
    ASSERT_TRUE(path.has_value());
    const std::vector<std::byte> content(1'000, std::byte{'q'});
    ASSERT_TRUE(builder.add(*path, content).has_value());
    const TempPath first_path("first.pak");
    const TempPath second_path("second.pak");
    ASSERT_TRUE(builder.write(first_path.path()).has_value());
    ASSERT_TRUE(builder.write(second_path.path()).has_value());
    Result<File> first_file = File::open(first_path.path());
    Result<File> second_file = File::open(second_path.path());
    ASSERT_TRUE(first_file.has_value() && second_file.has_value());
    Result<PakReader> first = PakReader::open_unverified(std::move(*first_file));
    Result<PakReader> second = PakReader::open_unverified(std::move(*second_file));
    ASSERT_TRUE(first.has_value() && second.has_value());
    *first = std::move(*second);
    PakReadContext context;
    PakReadContext moved_context = std::move(context);
    PakReadContext assigned_context;
    assigned_context = std::move(moved_context);
    std::vector<std::byte> out(content.size());
    const Result<usize> read = first->read(0, out, assigned_context);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(out, content);
}

}  // namespace
}  // namespace orion::io
