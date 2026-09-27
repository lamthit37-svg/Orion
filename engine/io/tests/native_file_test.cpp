#include "engine/io/detail/native_file.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/tests/support/temp_file.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace orion::io::detail {
namespace {

using testing::TempPath;

// Handle không trỏ tới tệp nào đang mở: mọi lời gọi gốc trả lỗi thay vì chết hay ghi bậy. Mã lỗi cụ
// thể tuỳ hệ điều hành (EBADF trên POSIX; trên Windows -1 là handle giả của tiến trình), nên chỉ
// kiểm rằng có lỗi.
TEST(NativeFile, OperationsOnAClosedHandleFail) {
    const NativeFile closed{};
    std::array<std::byte, 8> buffer{};
    EXPECT_FALSE(read_at(closed, 0, buffer).has_value());
    EXPECT_FALSE(file_size(closed).has_value());
    EXPECT_FALSE(write_all(closed, buffer).has_value());
    EXPECT_FALSE(sync_file(closed).has_value());
}

TEST(NativeFile, MissingFilesAreNotFound) {
    const TempPath missing("missing.bin");
    const TempPath other("other.bin");
    const Result<void> removed = remove_file(missing.path().c_str());
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code(), ErrorCode::NotFound);
    const Result<void> replaced = replace_file(missing.path().c_str(), other.path().c_str());
    ASSERT_FALSE(replaced.has_value());
    EXPECT_EQ(replaced.error().code(), ErrorCode::NotFound);
}

TEST(NativeFile, CreateRefusesToOverwrite) {
    const TempPath temp("data.bin");
    const Result<NativeFile> created = create_for_write(temp.path().c_str());
    ASSERT_TRUE(created.has_value());
    close_file(*created);
    const Result<NativeFile> again = create_for_write(temp.path().c_str());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
}

TEST(NativeFile, ReadingAtOrPastTheEndGivesZeroBytes) {
    const TempPath temp("data.bin");
    const Result<NativeFile> created = create_for_write(temp.path().c_str());
    ASSERT_TRUE(created.has_value());
    const std::array<std::byte, 4> content{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    ASSERT_TRUE(write_all(*created, content).has_value());
    close_file(*created);
    const Result<NativeFile> opened = open_for_read(temp.path().c_str());
    ASSERT_TRUE(opened.has_value());
    std::array<std::byte, 8> buffer{};
    const Result<usize> partial = read_at(*opened, 2, buffer);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(*partial, 2U);
    for (const u64 offset : {u64{4}, u64{1'000}}) {
        const Result<usize> end = read_at(*opened, offset, buffer);
        ASSERT_TRUE(end.has_value()) << offset;
        EXPECT_EQ(*end, 0U) << offset;
    }
    close_file(*opened);
}

}  // namespace
}  // namespace orion::io::detail
