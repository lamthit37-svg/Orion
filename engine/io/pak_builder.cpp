#include "engine/io/pak_builder.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#if ORION_DEV_TOOLS

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/io/file.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/path.hpp"

#include <zdict.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace detail {

// Bộ nén và dictionary đã chuẩn bị của một PakBuilder.
struct ZstdEncoder {
    ZstdEncoder() noexcept = default;
    ZstdEncoder(const ZstdEncoder&) = delete;
    ZstdEncoder& operator=(const ZstdEncoder&) = delete;
    ZstdEncoder(ZstdEncoder&&) = delete;
    ZstdEncoder& operator=(ZstdEncoder&&) = delete;
    ~ZstdEncoder() {
        for (ZSTD_CDict* dictionary : dictionaries) {
            ZSTD_freeCDict(dictionary);
        }
        ZSTD_freeCCtx(context);
    }

    ZSTD_CCtx* context = nullptr;
    std::vector<ZSTD_CDict*> dictionaries;
    i32 level = 1;
};

}  // namespace detail

namespace {

// ZDICT_DICTSIZE_MIN của zdict.h, nằm trong phần chỉ dành cho link tĩnh (ZDICT_STATIC_LINKING_ONLY)
// nên chép giá trị ra đây; zdict.h của v1.5.7 ghi 256.
constexpr usize kMinDictionaryCapacity = 256;

// Header và index của pak, tính từ các entry đã gom; blob đi thẳng từ entry.
struct Layout {
    std::array<std::byte, kPakHeaderSize> header{};
    std::vector<std::byte> index;
    u64 file_size = 0;
};

template <class Entries>
[[nodiscard]] Result<Layout> make_layout(const Entries& entries,
                                         const std::vector<std::vector<std::byte>>& dictionaries) {
    usize strings_size = 0;
    usize dictionary_bytes = 0;
    for (const auto& [path, pending] : entries) {
        strings_size += path.size();
    }
    for (const std::vector<std::byte>& dictionary : dictionaries) {
        dictionary_bytes += dictionary.size();
    }
    const usize index_size = (entries.size() * kPakEntrySize) +
                             (dictionaries.size() * kPakDictionaryRecordSize) + strings_size +
                             dictionary_bytes;
    if (index_size > kMaxPakIndexBytes) {
        return fail(ErrorCode::ResourceExhausted, "pak: index vượt 64 MiB",
                    static_cast<i64>(index_size));
    }

    Layout layout;
    layout.index.resize(index_size);
    core::ByteWriter index(layout.index);
    u64 offset = kPakHeaderSize;
    u32 path_offset = 0;
    for (const auto& [path, pending] : entries) {
        index.write<u64>(offset);
        index.write<u64>(pending.blob.size());
        index.write<u64>(pending.original_size);
        index.write<u32>(path_offset);
        index.write<u16>(static_cast<u16>(path.size()));
        index.write<u8>(pending.compression);
        index.write<u8>(pending.dictionary);
        index.write_bytes(pending.hash.bytes);
        offset += pending.blob.size();
        path_offset += static_cast<u32>(path.size());
    }
    u32 dictionary_offset = 0;
    for (const std::vector<std::byte>& dictionary : dictionaries) {
        index.write<u32>(dictionary_offset);
        index.write<u32>(static_cast<u32>(dictionary.size()));
        dictionary_offset += static_cast<u32>(dictionary.size());
    }
    for (const auto& [path, pending] : entries) {
        index.write_bytes(std::as_bytes(std::span(path)));
    }
    for (const std::vector<std::byte>& dictionary : dictionaries) {
        index.write_bytes(dictionary);
    }
    ORION_VERIFY(!index.overflowed() && index.offset() == index_size, "pak: tính sai cỡ index");

    core::ByteWriter header(layout.header);
    header.write_bytes(kPakMagic);
    header.write<u16>(kPakVersion);
    header.write<u16>(0);
    header.write<u32>(static_cast<u32>(entries.size()));
    header.write<u32>(static_cast<u32>(dictionaries.size()));
    header.write<u32>(static_cast<u32>(strings_size));
    header.write<u64>(offset);
    header.write<u64>(index_size);
    ORION_VERIFY(!header.overflowed(), "pak: tính sai cỡ header");
    layout.file_size = offset + index_size;
    return layout;
}

}  // namespace

PakBuilder::PakBuilder(const i32 compression_level)
    : encoder_(std::make_unique<detail::ZstdEncoder>()) {
    encoder_->level = std::clamp(compression_level, 1, ZSTD_maxCLevel());
    // NULL khi hết bộ nhớ; add báo ResourceExhausted khi đó.
    encoder_->context = ZSTD_createCCtx();
}

PakBuilder::PakBuilder(PakBuilder&& other) noexcept = default;
PakBuilder& PakBuilder::operator=(PakBuilder&& other) noexcept = default;
PakBuilder::~PakBuilder() = default;

Result<u8> PakBuilder::add_dictionary(const std::span<const std::byte> dictionary) {
    if (dictionary.empty() || dictionary.size() > kMaxPakDictionaryBytes) {
        return fail(ErrorCode::InvalidArgument, "pak: dictionary rỗng hay quá lớn",
                    static_cast<i64>(dictionary.size()));
    }
    if (dictionaries_.size() >= kMaxPakDictionaries) {
        return fail(ErrorCode::ResourceExhausted, "pak: đã đủ số dictionary");
    }
    // Kiểm luôn phía đọc, để pak ghi ra không bao giờ có dictionary mà PakReader từ chối.
    ZSTD_DDict* const readable = ZSTD_createDDict(dictionary.data(), dictionary.size());
    if (readable == nullptr) {
        return fail(ErrorCode::DataLoss, "pak: Zstd không nhận dictionary");
    }
    ZSTD_freeDDict(readable);
    ZSTD_CDict* const prepared =
        ZSTD_createCDict(dictionary.data(), dictionary.size(), encoder_->level);
    if (prepared == nullptr) {
        return fail(ErrorCode::DataLoss, "pak: Zstd không nhận dictionary");
    }
    encoder_->dictionaries.push_back(prepared);
    dictionaries_.emplace_back(dictionary.begin(), dictionary.end());
    return static_cast<u8>(dictionaries_.size());
}

Result<void> PakBuilder::add(const VirtualPath& path, const std::span<const std::byte> content,
                             const u8 dictionary) {
    if (entries_.size() >= kMaxPakEntries) {
        return fail(ErrorCode::ResourceExhausted, "pak: đã đủ số entry");
    }
    if (content.size() > kMaxPakEntryBytes || dictionary > dictionaries_.size()) {
        return fail(ErrorCode::InvalidArgument, "pak: entry quá lớn hay dictionary không có");
    }
    std::string key(path.view());
    if (entries_.contains(key)) {
        return fail(ErrorCode::AlreadyExists, "pak: đường dẫn đã có trong pak");
    }
    if (encoder_->context == nullptr) {
        return fail(ErrorCode::ResourceExhausted, "pak: không tạo được bộ nén");
    }
    std::vector<std::byte> compressed(ZSTD_compressBound(content.size()));
    const usize size =
        dictionary == 0
            ? ZSTD_compressCCtx(encoder_->context, compressed.data(), compressed.size(),
                                content.data(), content.size(), encoder_->level)
            : ZSTD_compress_usingCDict(encoder_->context, compressed.data(), compressed.size(),
                                       content.data(), content.size(),
                                       encoder_->dictionaries[dictionary - 1U]);
    // Đích đã đủ ZSTD_compressBound nên lỗi ở đây chỉ còn là hết bộ nhớ hay tham số hỏng.
    // ZSTD_getErrorCode không được gọi: từ v1.5.7 nó chuyển từ zstd_errors.h sang zstd.h.
    if (ZSTD_isError(size) != 0U) {
        return fail(ErrorCode::Internal, "pak: Zstd nén thất bại");
    }
    Pending pending;
    pending.original_size = content.size();
    if (size < content.size()) {
        compressed.resize(size);
        pending.blob = std::move(compressed);
        pending.compression = static_cast<u8>(PakCompression::Zstd);
        pending.dictionary = dictionary;
    } else {
        pending.blob.assign(content.begin(), content.end());
    }
    pending.hash = crypto::hash(pending.blob);
    entries_.emplace(std::move(key), std::move(pending));
    return {};
}

Result<std::vector<std::byte>> PakBuilder::build() const {
    const Result<Layout> layout = make_layout(entries_, dictionaries_);
    if (!layout) {
        return std::unexpected(layout.error());
    }
    std::vector<std::byte> pak;
    pak.reserve(static_cast<usize>(layout->file_size));
    pak.insert(pak.end(), layout->header.begin(), layout->header.end());
    for (const auto& [path, pending] : entries_) {
        pak.insert(pak.end(), pending.blob.begin(), pending.blob.end());
    }
    pak.insert(pak.end(), layout->index.begin(), layout->index.end());
    return pak;
}

Result<PakSummary> PakBuilder::write(const std::string_view path) const {
    const Result<Layout> layout = make_layout(entries_, dictionaries_);
    if (!layout) {
        return std::unexpected(layout.error());
    }
    Result<AtomicFileWriter> writer = AtomicFileWriter::create(path);
    if (!writer) {
        return std::unexpected(writer.error());
    }
    crypto::Hasher file_hasher;
    const auto emit = [&](const std::span<const std::byte> bytes) {
        file_hasher.update(bytes);
        return writer->write(bytes);
    };
    if (const Result<void> written = emit(layout->header); !written) {
        return std::unexpected(written.error());
    }
    for (const auto& [entry_path, pending] : entries_) {
        if (const Result<void> written = emit(pending.blob); !written) {
            return std::unexpected(written.error());
        }
    }
    if (const Result<void> written = emit(layout->index); !written) {
        return std::unexpected(written.error());
    }
    if (const Result<void> committed = writer->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    crypto::Hasher index_hasher;
    index_hasher.update(layout->header);
    index_hasher.update(layout->index);
    return PakSummary{file_hasher.finish(), index_hasher.finish(), layout->file_size,
                      static_cast<u32>(entries_.size())};
}

Result<std::vector<std::byte>> train_pak_dictionary(
    const std::span<const std::span<const std::byte>> samples, const usize capacity) {
    if (samples.empty() || samples.size() > std::numeric_limits<unsigned>::max() ||
        capacity < kMinDictionaryCapacity || capacity > kMaxPakDictionaryBytes) {
        return fail(ErrorCode::InvalidArgument, "pak: mẫu hay cỡ dictionary ngoài khoảng");
    }
    std::vector<std::byte> joined;
    std::vector<usize> sizes;
    sizes.reserve(samples.size());
    for (const std::span<const std::byte> sample : samples) {
        joined.insert(joined.end(), sample.begin(), sample.end());
        sizes.push_back(sample.size());
    }
    std::vector<std::byte> dictionary(capacity);
    const usize size = ZDICT_trainFromBuffer(dictionary.data(), dictionary.size(), joined.data(),
                                             sizes.data(), static_cast<unsigned>(samples.size()));
    if (ZDICT_isError(size) != 0U) {
        return fail(ErrorCode::DataLoss, "pak: Zstd không huấn luyện được dictionary");
    }
    dictionary.resize(size);
    return dictionary;
}

}  // namespace orion::io

#endif  // ORION_DEV_TOOLS
