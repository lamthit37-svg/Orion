#pragma once

// Ghi pak (docs/formats/pak.md) cho cooker và test. Chỉ có khi ORION_DEV_TOOLS: bản ship chỉ đọc
// pak (CLAUDE.md X.9).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#if ORION_DEV_TOOLS

#include "engine/crypto/hash.hpp"
#include "engine/io/path.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orion::io {

// Những gì manifest cần về một pak vừa ghi.
struct PakSummary {
    crypto::Hash file_hash;
    crypto::Hash index_hash;
    u64 file_size = 0;
    u32 entry_count = 0;
};

namespace detail {
struct ZstdEncoder;
}  // namespace detail

// Gom entry trong bộ nhớ rồi ghi một pak. Cùng entry, dictionary và mức nén luôn cho cùng dãy byte
// (entry xếp theo đường dẫn, không có dấu thời gian), nên tên pak theo hash ổn định giữa các lần
// cook. Chỉ dùng từ một luồng.
class PakBuilder {
public:
    // Mức nén Zstd từ 1 tới 22 (ZSTD_maxCLevel()); ngoài khoảng thì bị kẹp lại.
    explicit PakBuilder(i32 compression_level);
    PakBuilder(const PakBuilder&) = delete;
    PakBuilder& operator=(const PakBuilder&) = delete;
    PakBuilder(PakBuilder&& other) noexcept;
    PakBuilder& operator=(PakBuilder&& other) noexcept;
    ~PakBuilder();

    // Thêm một dictionary Zstd (từ train_pak_dictionary, hoặc nội dung thô); trả số thứ tự k, đếm
    // từ 1, để truyền cho add. Lỗi: InvalidArgument khi rỗng hay lớn hơn kMaxPakDictionaryBytes,
    // ResourceExhausted khi đã đủ kMaxPakDictionaries, DataLoss khi Zstd không nhận dictionary.
    [[nodiscard]] Result<u8> add_dictionary(std::span<const std::byte> dictionary);

    // Nén `content` (bằng dictionary k khi k khác 0); bản nén không nhỏ hơn bản gốc thì lưu thẳng.
    // Lỗi: AlreadyExists khi trùng đường dẫn, InvalidArgument khi k không có hay content lớn hơn
    // kMaxPakEntryBytes, ResourceExhausted khi đã đủ kMaxPakEntries.
    [[nodiscard]] Result<void> add(const VirtualPath& path, std::span<const std::byte> content,
                                   u8 dictionary = 0);

    // Toàn bộ tệp pak trong bộ nhớ.
    [[nodiscard]] Result<std::vector<std::byte>> build() const;
    // Ghi pak ra `path` bằng AtomicFileWriter.
    [[nodiscard]] Result<PakSummary> write(std::string_view path) const;

private:
    struct Pending {
        std::vector<std::byte> blob;
        u64 original_size = 0;
        u8 compression = 0;
        u8 dictionary = 0;
        crypto::Hash hash;
    };

    std::map<std::string, Pending> entries_;
    std::vector<std::vector<std::byte>> dictionaries_;
    std::unique_ptr<detail::ZstdEncoder> encoder_;
};

// Huấn luyện dictionary Zstd tối đa `capacity` byte từ các mẫu (ZDICT_trainFromBuffer). Lỗi:
// InvalidArgument khi không có mẫu hay capacity ngoài khoảng, DataLoss khi Zstd không huấn luyện
// được (ví dụ quá ít mẫu).
[[nodiscard]] Result<std::vector<std::byte>> train_pak_dictionary(
    std::span<const std::span<const std::byte>> samples, usize capacity);

}  // namespace orion::io

#endif  // ORION_DEV_TOOLS
