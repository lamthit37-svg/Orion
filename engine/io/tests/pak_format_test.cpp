// Luật của docs/formats/pak.md: mỗi test sửa đúng một trường của một pak hợp lệ và mong đúng mã lỗi
// của bảng "Lỗi".

#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/pak_builder.hpp"
#include "engine/io/path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

class PakFormatTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(crypto::initialize().has_value());
        PakBuilder builder(3);
        const std::vector<std::byte> dictionary(600, std::byte{'d'});
        ASSERT_TRUE(builder.add_dictionary(dictionary).has_value());
        for (const std::string_view path : {"a.bin", "b/c.bin", "d.txt"}) {
            const Result<VirtualPath> parsed = VirtualPath::parse(path);
            ASSERT_TRUE(parsed.has_value());
            const std::vector<std::byte> content(1'000, std::byte{'x'});
            ASSERT_TRUE(builder.add(*parsed, content, path == "d.txt" ? 1 : 0).has_value());
        }
        Result<std::vector<std::byte>> image = builder.build();
        ASSERT_TRUE(image.has_value());
        image_ = std::move(*image);
        ASSERT_TRUE(PakIndex::parse_image(image_).has_value());
    }

    [[nodiscard]] u64 index_offset() const {
        return core::load_le<u64>(std::span(image_).subspan(24).first<8>());
    }
    [[nodiscard]] usize entry_field(const usize entry, const usize field) const {
        return static_cast<usize>(index_offset()) + (entry * kPakEntrySize) + field;
    }

    template <class T>
    void store(const usize offset, const T value) {
        core::store_le<T>(std::span(image_).subspan(offset).template first<sizeof(T)>(), value);
    }
    template <class T>
    [[nodiscard]] T load(const usize offset) const {
        return core::load_le<T>(std::span(image_).subspan(offset).template first<sizeof(T)>());
    }

    [[nodiscard]] std::vector<std::byte>& image() noexcept { return image_; }

    // Sửa ảnh pak bằng `mutate` rồi mong parse_image trả `expected`.
    void expect_rejected(const std::function<void()>& mutate, const ErrorCode expected) {
        const std::vector<std::byte> original = image_;
        mutate();
        const Result<PakIndex> index = PakIndex::parse_image(image_);
        ASSERT_FALSE(index.has_value());
        EXPECT_EQ(index.error().code(), expected) << index.error().context();
        image_ = original;
    }

private:
    std::vector<std::byte> image_;
};

TEST_F(PakFormatTest, HeaderRules) {
    expect_rejected([&] { image()[0] = std::byte{'X'}; }, ErrorCode::InvalidArgument);
    expect_rejected([&] { store<u16>(8, 2); }, ErrorCode::Unimplemented);
    expect_rejected([&] { store<u16>(10, 1); }, ErrorCode::DataLoss);
    expect_rejected([&] { image()[63] = std::byte{1}; }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u32>(12, kMaxPakEntries + 1); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u32>(16, kMaxPakDictionaries + 1); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u64>(24, 63); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u64>(24, index_offset() + 1); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u64>(32, load<u64>(32) - 1); }, ErrorCode::DataLoss);
    expect_rejected([&] { image().push_back(std::byte{0}); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u32>(20, 0xFFFF'FFFFU); }, ErrorCode::DataLoss);
    expect_rejected([&] { image().resize(10); }, ErrorCode::InvalidArgument);
}

TEST_F(PakFormatTest, EntryRules) {
    // Blob chồng lên header, hay tràn sang index.
    expect_rejected([&] { store<u64>(entry_field(0, 0), 10); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u64>(entry_field(0, 0), index_offset() + 1); },
                    ErrorCode::DataLoss);
    expect_rejected([&] { store<u64>(entry_field(0, 8), index_offset()); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u64>(entry_field(0, 16), kMaxPakEntryBytes + 1); },
                    ErrorCode::DataLoss);
    // Kiểu nén lạ; entry không nén mà cỡ lệch hay có dictionary; dictionary không có.
    expect_rejected([&] { image()[entry_field(0, 30)] = std::byte{2}; }, ErrorCode::DataLoss);
    expect_rejected(
        [&] {
            image()[entry_field(0, 30)] = std::byte{0};
            store<u64>(entry_field(0, 16), load<u64>(entry_field(0, 8)) + 1);
        },
        ErrorCode::DataLoss);
    expect_rejected(
        [&] {
            image()[entry_field(0, 30)] = std::byte{0};
            store<u64>(entry_field(0, 16), load<u64>(entry_field(0, 8)));
            image()[entry_field(0, 31)] = std::byte{1};
        },
        ErrorCode::DataLoss);
    expect_rejected([&] { image()[entry_field(2, 31)] = std::byte{2}; }, ErrorCode::DataLoss);
    // Đường dẫn ngoài bảng chuỗi, sai luật, hay làm hỏng thứ tự.
    expect_rejected([&] { store<u32>(entry_field(0, 24), 0xFFFF'FF00U); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u16>(entry_field(0, 28), 0); }, ErrorCode::DataLoss);
    // "b/c.bin" cắt còn "b/": kết thúc bằng '/'.
    expect_rejected([&] { store<u16>(entry_field(1, 28), 2); }, ErrorCode::DataLoss);
    expect_rejected(
        [&] {
            // Entry 0 và 1 cùng trỏ tới đường dẫn "a.bin": trùng nên không tăng ngặt.
            store<u32>(entry_field(1, 24), load<u32>(entry_field(0, 24)));
            store<u16>(entry_field(1, 28), load<u16>(entry_field(0, 28)));
        },
        ErrorCode::DataLoss);
}

TEST_F(PakFormatTest, DictionaryRules) {
    const usize record = static_cast<usize>(index_offset()) + (3 * kPakEntrySize);
    expect_rejected([&] { store<u32>(record + 4, 0); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u32>(record + 4, kMaxPakDictionaryBytes + 1); },
                    ErrorCode::DataLoss);
    expect_rejected([&] { store<u32>(record, 1); }, ErrorCode::DataLoss);
    expect_rejected([&] { store<u32>(record, 0xFFFF'FFFFU); }, ErrorCode::DataLoss);
}

// Blob quá 1 GiB bị từ chối kể cả khi tệp đủ lớn để chứa nó. PakIndex::parse nhận index riêng nên
// tệp 4 GiB không cần có thật: header chỉ dời index_offset ra xa.
TEST_F(PakFormatTest, StoredSizeIsCappedEvenWhenTheFileIsLargeEnough) {
    const Result<PakIndex> original = PakIndex::parse_image(image());
    ASSERT_TRUE(original.has_value());
    ASSERT_EQ(original->entry(0).compression, PakCompression::Zstd);
    std::array<std::byte, kPakHeaderSize> header{};
    std::ranges::copy(std::span(std::as_const(image())).first<kPakHeaderSize>(), header.begin());
    const std::vector<std::byte> index(
        image().begin() + static_cast<std::ptrdiff_t>(index_offset()), image().end());
    const u64 far_index_offset = u64{4} << 30U;
    core::store_le<u64>(std::span(header).subspan<24, 8>(), far_index_offset);
    const auto parse_with_stored_size = [&](const u64 stored_size) {
        std::vector<std::byte> changed = index;
        core::store_le<u64>(std::span(changed).subspan<8, 8>(), stored_size);
        return PakIndex::parse(header, std::move(changed), far_index_offset + index.size());
    };
    EXPECT_TRUE(parse_with_stored_size(kMaxPakEntryBytes).has_value());
    const Result<PakIndex> too_large = parse_with_stored_size(kMaxPakEntryBytes + 1);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().code(), ErrorCode::DataLoss);
}

TEST_F(PakFormatTest, ParseChecksIndexSizeAgainstHeader) {
    const std::span<const std::byte, kPakHeaderSize> header =
        std::span(std::as_const(image())).first<kPakHeaderSize>();
    const std::vector<std::byte> short_index(10);
    const Result<PakIndex> index = PakIndex::parse(header, short_index, image().size());
    ASSERT_FALSE(index.has_value());
    EXPECT_EQ(index.error().code(), ErrorCode::DataLoss);
}

}  // namespace
}  // namespace orion::io
