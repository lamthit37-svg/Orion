#pragma once

// Pak (docs/formats/pak.md): nhiều tệp cooked trong một tệp, mỗi entry nén Zstd riêng.
//
// - PakIndex phân tích header và index: hàm toàn phần trên dữ liệu từ ngoài (X.5, X.9), có fuzz
//   target `io_pak_index`.
// - PakReader mở tệp pak, so hash của header và index với manifest đã ký, rồi đọc entry: băm blob
//   và so với index trước khi giải nén.
// - PakReadContext giữ bộ đệm và bộ giải nén của một luồng đọc.
//
// Ghi pak ở pak_builder.hpp (chỉ có khi ORION_DEV_TOOLS).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/io/file.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace orion::io {

inline constexpr std::array<std::byte, 8> kPakMagic = {
    std::byte{'O'}, std::byte{'R'}, std::byte{'I'}, std::byte{'O'},
    std::byte{'N'}, std::byte{'P'}, std::byte{'A'}, std::byte{'K'}};
inline constexpr u16 kPakVersion = 1;
inline constexpr usize kPakHeaderSize = 64;
inline constexpr usize kPakEntrySize = 64;
inline constexpr usize kPakDictionaryRecordSize = 8;
inline constexpr u32 kMaxPakEntries = 262'144;
inline constexpr u32 kMaxPakDictionaries = 16;
inline constexpr u32 kMaxPakDictionaryBytes = u32{1} << 20U;
inline constexpr u64 kMaxPakIndexBytes = u64{64} << 20U;
// Giới hạn của cả original_size lẫn stored_size.
inline constexpr u64 kMaxPakEntryBytes = u64{1} << 30U;

enum class PakCompression : u8 {
    None = 0,
    Zstd = 1,
};

// Một entry đã qua mọi luật của định dạng.
struct PakEntry {
    u64 offset = 0;
    u64 stored_size = 0;
    u64 original_size = 0;
    u32 path_offset = 0;
    u16 path_length = 0;
    PakCompression compression = PakCompression::None;
    // 0: không dùng; k: dictionary thứ k, đếm từ 1.
    u8 dictionary = 0;
    crypto::Hash hash;
};

// Phần header cần để tìm index.
struct PakHeader {
    u32 entry_count = 0;
    u32 dictionary_count = 0;
    u32 string_table_size = 0;
    u64 index_offset = 0;
    u64 index_size = 0;
};

// Kiểm header của một tệp cỡ `file_size`. Lỗi: InvalidArgument (không phải pak), Unimplemented
// (phiên bản khác), DataLoss (vi phạm luật khác).
[[nodiscard]] Result<PakHeader> parse_pak_header(std::span<const std::byte, kPakHeaderSize> header,
                                                 u64 file_size) noexcept;

// Index đã kiểm mọi luật của định dạng; sở hữu vùng index. Bất biến sau khi dựng, nên đọc từ nhiều
// luồng cùng lúc được.
class PakIndex {
public:
    // `header` là 64 byte đầu tệp, `index` là đúng index_size byte ở index_offset của một tệp cỡ
    // `file_size`. Hàm toàn phần: mọi đầu vào cho ra index hợp lệ hoặc lỗi như parse_pak_header.
    // O(n log n) theo số entry (kiểm thứ tự), không kể phần kiểm đường dẫn.
    [[nodiscard]] static Result<PakIndex> parse(std::span<const std::byte, kPakHeaderSize> header,
                                                std::vector<std::byte> index,
                                                u64 file_size) noexcept;
    // Như parse, cho cả tệp pak nằm trong bộ nhớ (fuzz target, test, kiểm tệp vừa tải về).
    [[nodiscard]] static Result<PakIndex> parse_image(std::span<const std::byte> image) noexcept;

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }
    // i < entry_count().
    [[nodiscard]] const PakEntry& entry(usize i) const noexcept;
    [[nodiscard]] std::string_view path(usize i) const noexcept;
    // Tìm nhị phân theo đường dẫn, O(log n).
    [[nodiscard]] std::optional<usize> find(std::string_view path) const noexcept;

    [[nodiscard]] usize dictionary_count() const noexcept { return dictionaries_.size(); }
    // 1 <= k <= dictionary_count().
    [[nodiscard]] std::span<const std::byte> dictionary(usize k) const noexcept;

    // Blob lớn nhất: đủ chỗ bộ đệm để đọc mọi entry mà không cấp phát (PakReadContext::reserve).
    [[nodiscard]] u64 max_stored_size() const noexcept { return max_stored_size_; }

private:
    struct DictionaryRange {
        usize offset = 0;
        usize size = 0;
    };

    PakIndex() = default;

    std::vector<std::byte> index_;
    std::vector<PakEntry> entries_;
    std::vector<DictionaryRange> dictionaries_;
    usize strings_offset_ = 0;
    u64 max_stored_size_ = 0;
};

namespace detail {
struct ZstdDecoder;
struct PakDictionaries;
}  // namespace detail

// Bộ đệm và bộ giải nén cho một luồng đọc pak. Một context chỉ được dùng bởi một luồng tại một thời
// điểm; mỗi luồng đọc có context riêng.
class PakReadContext {
public:
    PakReadContext() noexcept;
    PakReadContext(const PakReadContext&) = delete;
    PakReadContext& operator=(const PakReadContext&) = delete;
    PakReadContext(PakReadContext&& other) noexcept;
    PakReadContext& operator=(PakReadContext&& other) noexcept;
    ~PakReadContext();

    // Dành sẵn bộ đệm cho blob tới `stored_size` byte, để các lần đọc sau không cấp phát (X.7).
    void reserve(u64 stored_size);

private:
    friend class PakReader;

    std::vector<std::byte> scratch_;
    std::unique_ptr<detail::ZstdDecoder> decoder_;
};

class PakReader {
public:
    // Mở pak mà manifest đã ký mô tả: cỡ tệp phải bằng `expected_file_size` và hash của header nối
    // index phải bằng `expected_index_hash`, trước khi index được phân tích. Lỗi: DataLoss khi
    // lệch hay khi dictionary không dựng được; lỗi của File; lỗi của PakIndex::parse.
    [[nodiscard]] static Result<PakReader> open(File file, u64 expected_file_size,
                                                const crypto::Hash& expected_index_hash) noexcept;

#if ORION_DEV_TOOLS
    // Chỉ DEV và tool: không có manifest để so index. Hash của từng entry vẫn được kiểm khi đọc.
    [[nodiscard]] static Result<PakReader> open_unverified(File file) noexcept;
#endif

    PakReader(const PakReader&) = delete;
    PakReader& operator=(const PakReader&) = delete;
    PakReader(PakReader&& other) noexcept;
    PakReader& operator=(PakReader&& other) noexcept;
    ~PakReader();

    [[nodiscard]] const PakIndex& index() const noexcept { return index_; }

    // Đọc entry `i` vào đầu `out` và trả original_size. Blob được băm và so với index trước khi
    // giải nén. Lỗi: InvalidArgument (i ngoài khoảng, out nhỏ hơn original_size), DataLoss (hash
    // sai, frame Zstd sai), lỗi của File. Khi lỗi, nội dung của out không xác định. Gọi từ nhiều
    // luồng cùng lúc được, mỗi luồng một context.
    [[nodiscard]] Result<usize> read(usize i, std::span<std::byte> out,
                                     PakReadContext& context) const noexcept;

private:
    PakReader(File file, PakIndex index,
              std::unique_ptr<detail::PakDictionaries> dictionaries) noexcept;
    [[nodiscard]] static Result<PakReader> open_checked(File file,
                                                        const crypto::Hash* expected) noexcept;

    File file_;
    PakIndex index_;
    std::unique_ptr<detail::PakDictionaries> dictionaries_;
};

}  // namespace orion::io
