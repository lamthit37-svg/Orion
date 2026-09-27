#include "engine/io/file.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "engine/jobs/thread.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

using testing::TempPath;

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> pattern(const usize size) {
    std::vector<std::byte> bytes(size);
    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 131U + 7U) & 0xFFU);
    }
    return bytes;
}

void write_file(const std::string& path, const std::span<const std::byte> content) {
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(path);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->write(content).has_value());
    ASSERT_TRUE(writer->commit().has_value());
}

[[nodiscard]] std::vector<std::byte> read_all(const File& file) {
    std::vector<std::byte> content(static_cast<usize>(file.size()));
    EXPECT_TRUE(file.read_exact(0, content).has_value());
    return content;
}

TEST(File, WritesAtomicallyAndReadsBack) {
    const TempPath temp("data.bin");
    const std::vector<std::byte> content = pattern(100'000);
    write_file(temp.path(), content);
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->size(), content.size());
    EXPECT_EQ(read_all(*file), content);
}

TEST(File, ReadsAtAnyOffset) {
    const TempPath temp("data.bin");
    const std::vector<std::byte> content = pattern(4'096);
    write_file(temp.path(), content);
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    std::array<std::byte, 100> slice{};
    ASSERT_TRUE(file->read_exact(3'000, slice).has_value());
    EXPECT_TRUE(std::ranges::equal(slice, std::span(content).subspan(3'000, slice.size())));
    EXPECT_TRUE(file->read_exact(4'096, {}).has_value());
    ASSERT_TRUE(file->read_exact(3'996, slice).has_value());
}

TEST(File, ReadingPastTheEndIsOutOfRange) {
    const TempPath temp("data.bin");
    write_file(temp.path(), pattern(10));
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    std::array<std::byte, 4> buffer{};
    for (const u64 offset : {u64{7}, u64{11}, ~u64{0}}) {
        const Result<void> read = file->read_exact(offset, buffer);
        ASSERT_FALSE(read.has_value()) << offset;
        EXPECT_EQ(read.error().code(), ErrorCode::OutOfRange);
    }
}

TEST(File, EmptyFileHasSizeZero) {
    const TempPath temp("empty.bin");
    write_file(temp.path(), {});
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->size(), 0U);
    EXPECT_TRUE(file->read_exact(0, {}).has_value());
}

TEST(File, OpenReportsWhyItFailed) {
    const TempPath missing("missing.bin");
    const Result<File> absent = File::open(missing.path());
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().code(), ErrorCode::NotFound);

    // Thư mục không phải tệp thường.
    const Result<File> directory = File::open(::testing::TempDir());
    ASSERT_FALSE(directory.has_value());

    for (const std::string_view bad :
         {std::string_view{}, std::string_view("a\0b", 3), std::string_view("\xFF\xFE")}) {
        const Result<File> file = File::open(bad);
        ASSERT_FALSE(file.has_value());
        EXPECT_EQ(file.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(File, Utf8PathsWork) {
    const TempPath temp("tệp_đọc_ghi.bin");
    const std::vector<std::byte> content = bytes_of("xin chào");
    write_file(temp.path(), content);
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(read_all(*file), content);
}

TEST(File, MoveTransfersTheHandle) {
    const TempPath temp("data.bin");
    write_file(temp.path(), bytes_of("abc"));
    Result<File> opened = File::open(temp.path());
    ASSERT_TRUE(opened.has_value());
    File moved = std::move(*opened);
    EXPECT_EQ(read_all(moved), bytes_of("abc"));
    const File assigned = std::move(moved);
    EXPECT_EQ(read_all(assigned), bytes_of("abc"));
}

TEST(File, MoveAssignmentClosesThePreviousFile) {
    const TempPath first_path("first.bin");
    const TempPath second_path("second.bin");
    write_file(first_path.path(), bytes_of("first"));
    write_file(second_path.path(), bytes_of("second"));
    Result<File> first = File::open(first_path.path());
    Result<File> second = File::open(second_path.path());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    *first = std::move(*second);
    EXPECT_EQ(read_all(*first), bytes_of("second"));
}

TEST(AtomicFileWriter, ReplacesTargetEvenWhileItIsOpenForReading) {
    const TempPath temp("data.bin");
    write_file(temp.path(), bytes_of("old"));
    const Result<File> old_file = File::open(temp.path());
    ASSERT_TRUE(old_file.has_value());
    write_file(temp.path(), bytes_of("new content"));
    // Handle mở trước vẫn đọc nội dung cũ; mở lại thì thấy nội dung mới.
    EXPECT_EQ(read_all(*old_file), bytes_of("old"));
    const Result<File> new_file = File::open(temp.path());
    ASSERT_TRUE(new_file.has_value());
    EXPECT_EQ(read_all(*new_file), bytes_of("new content"));
}

TEST(AtomicFileWriter, DroppingWithoutCommitLeavesTargetUntouched) {
    const TempPath temp("data.bin");
    write_file(temp.path(), bytes_of("keep"));
    {
        Result<AtomicFileWriter> writer = AtomicFileWriter::create(temp.path());
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->write(bytes_of("half written")).has_value());
    }
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(read_all(*file), bytes_of("keep"));
    // Tệp tạm đầu tiên đã bị xoá, nên writer kế lấy lại đúng tên đó.
    const Result<File> leftover = File::open(temp.path() + ".tmp0");
    ASSERT_FALSE(leftover.has_value());
    EXPECT_EQ(leftover.error().code(), ErrorCode::NotFound);
}

TEST(AtomicFileWriter, ConcurrentWritersUseDistinctTemporaries) {
    const TempPath temp("data.bin");
    Result<AtomicFileWriter> first = AtomicFileWriter::create(temp.path());
    Result<AtomicFileWriter> second = AtomicFileWriter::create(temp.path());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(first->write(bytes_of("first")).has_value());
    ASSERT_TRUE(second->write(bytes_of("second")).has_value());
    ASSERT_TRUE(first->commit().has_value());
    ASSERT_TRUE(second->commit().has_value());
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(read_all(*file), bytes_of("second"));
}

TEST(AtomicFileWriter, UseAfterCommitIsFailedPrecondition) {
    const TempPath temp("data.bin");
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(temp.path());
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->commit().has_value());
    const Result<void> again = writer->commit();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::FailedPrecondition);
    const Result<void> write = writer->write(bytes_of("x"));
    ASSERT_FALSE(write.has_value());
    EXPECT_EQ(write.error().code(), ErrorCode::FailedPrecondition);
}

TEST(AtomicFileWriter, MoveAssignmentDiscardsTheReplacedWriter) {
    const TempPath first_path("first.bin");
    const TempPath second_path("second.bin");
    Result<AtomicFileWriter> first = AtomicFileWriter::create(first_path.path());
    Result<AtomicFileWriter> second = AtomicFileWriter::create(second_path.path());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(first->write(bytes_of("dropped")).has_value());
    ASSERT_TRUE(second->write(bytes_of("kept")).has_value());
    *first = std::move(*second);
    ASSERT_TRUE(first->commit().has_value());
    const Result<File> dropped = File::open(first_path.path());
    ASSERT_FALSE(dropped.has_value());
    EXPECT_EQ(dropped.error().code(), ErrorCode::NotFound);
    const Result<File> kept = File::open(second_path.path());
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(read_all(*kept), bytes_of("kept"));
}

TEST(AtomicFileWriter, InvalidPathIsRejected) {
    const Result<AtomicFileWriter> writer = AtomicFileWriter::create(std::string_view("a\0b", 3));
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().code(), ErrorCode::InvalidArgument);
}

// Mỗi writer đang mở giữ một tên tệp tạm; khi mọi tên đã có người giữ thì writer mới bỏ cuộc thay
// vì ghi đè tệp tạm của writer khác.
TEST(AtomicFileWriter, GivesUpWhenEveryTemporaryNameIsTaken) {
    const TempPath temp("data.bin");
    std::vector<AtomicFileWriter> holders;
    while (true) {
        Result<AtomicFileWriter> writer = AtomicFileWriter::create(temp.path());
        if (!writer.has_value()) {
            EXPECT_EQ(writer.error().code(), ErrorCode::AlreadyExists);
            break;
        }
        holders.push_back(std::move(*writer));
        ASSERT_LE(holders.size(), 64U);
    }
    EXPECT_EQ(holders.size(), 16U);
}

TEST(AtomicFileWriter, MissingDirectoryIsNotFound) {
    const std::string path = ::testing::TempDir() + "orion_io_no_such_dir/child/data.bin";
    const Result<AtomicFileWriter> writer = AtomicFileWriter::create(path);
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().code(), ErrorCode::NotFound);
}

// Nhiều luồng đọc chung một File cùng lúc: read_exact theo vị trí, không có trạng thái dùng chung.
// Chạy dưới TSan ở preset linux-tsan.
TEST(File, ConcurrentPositionalReadsAreIndependent) {
    const TempPath temp("data.bin");
    const std::vector<std::byte> content = pattern(usize{64} * 1'024);
    write_file(temp.path(), content);
    const Result<File> file = File::open(temp.path());
    ASSERT_TRUE(file.has_value());
    std::atomic<u32> mismatches{0};
    std::vector<jobs::Thread> readers;
    for (u32 t = 0; t < 4; ++t) {
        Result<jobs::Thread> reader =
            jobs::Thread::start("io-reader", [&, t](jobs::StopToken /*token*/) {
                std::array<std::byte, 1'000> buffer{};
                for (u32 round = 0; round < 200; ++round) {
                    const usize offset = static_cast<usize>(((round * 4U) + t) * 61U) %
                                         (content.size() - buffer.size());
                    if (!file->read_exact(offset, buffer).has_value() ||
                        !std::ranges::equal(buffer,
                                            std::span(content).subspan(offset, buffer.size()))) {
                        mismatches.fetch_add(1);
                    }
                }
            });
        ASSERT_TRUE(reader.has_value());
        readers.push_back(std::move(*reader));
    }
    readers.clear();
    EXPECT_EQ(mismatches.load(), 0U);
}

}  // namespace
}  // namespace orion::io
