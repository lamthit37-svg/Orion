#include "engine/io/pak.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/crypto/hash.hpp"
#include "engine/io/file.hpp"
#include "engine/io/path.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace detail {

// Bộ giải nén của một PakReadContext; tạo khi context đọc blob Zstd đầu tiên.
struct ZstdDecoder {
    ZstdDecoder() noexcept = default;
    ZstdDecoder(const ZstdDecoder&) = delete;
    ZstdDecoder& operator=(const ZstdDecoder&) = delete;
    ZstdDecoder(ZstdDecoder&&) = delete;
    ZstdDecoder& operator=(ZstdDecoder&&) = delete;
    ~ZstdDecoder() { ZSTD_freeDCtx(context); }

    ZSTD_DCtx* context = nullptr;
};

// Dictionary của một pak, dựng một lần khi mở rồi dùng chung giữa các luồng đọc qua
// const ZSTD_DDict*, không khoá. zstd.h chỉ nói rõ điều này cho CDict; với DDict đã đo ở
// NGHI-NGO-033.
struct PakDictionaries {
    struct Free {
        void operator()(ZSTD_DDict* dictionary) const noexcept { ZSTD_freeDDict(dictionary); }
    };

    std::vector<std::unique_ptr<ZSTD_DDict, Free>> dictionaries;
};

}  // namespace detail

namespace {

// Magic của frame Zstd định dạng hiện hành; frame v0.x cũ và frame bỏ qua (skippable) bị từ chối
// (ADR 0012).
constexpr u32 kZstdFrameMagic = 0xFD2F'B528U;

template <usize Offset, usize Size>
[[nodiscard]] std::span<const std::byte, Size> field(const std::span<const std::byte> bytes) {
    return bytes.subspan(Offset).first<Size>();
}

template <std::integral T, usize Offset>
[[nodiscard]] T read_field(const std::span<const std::byte> bytes) {
    return core::load_le<T>(field<Offset, sizeof(T)>(bytes));
}

[[nodiscard]] std::unexpected<Error> corrupt(const ErrorContext context,
                                             const i64 detail = 0) noexcept {
    return fail(ErrorCode::DataLoss, context, detail);
}

// Kiểm một bản ghi entry; `strings` là bảng chuỗi, `index_offset` là đầu index trong tệp.
[[nodiscard]] Result<PakEntry> parse_entry(const std::span<const std::byte> record,
                                           const std::string_view strings,
                                           const PakHeader& header) noexcept {
    PakEntry entry;
    entry.offset = read_field<u64, 0>(record);
    entry.stored_size = read_field<u64, 8>(record);
    entry.original_size = read_field<u64, 16>(record);
    entry.path_offset = read_field<u32, 24>(record);
    entry.path_length = read_field<u16, 28>(record);
    const auto compression = read_field<u8, 30>(record);
    entry.dictionary = read_field<u8, 31>(record);
    std::ranges::copy(field<32, crypto::kHashSize>(record), entry.hash.bytes.begin());

    if (entry.offset < kPakHeaderSize || entry.offset > header.index_offset ||
        entry.stored_size > header.index_offset - entry.offset) {
        return corrupt("pak: blob nằm ngoài vùng dữ liệu");
    }
    if (entry.original_size > kMaxPakEntryBytes || entry.stored_size > kMaxPakEntryBytes) {
        return corrupt("pak: entry lớn quá giới hạn");
    }
    if (compression == static_cast<u8>(PakCompression::None)) {
        entry.compression = PakCompression::None;
        if (entry.stored_size != entry.original_size || entry.dictionary != 0) {
            return corrupt("pak: entry không nén có cỡ hay dictionary sai");
        }
    } else if (compression == static_cast<u8>(PakCompression::Zstd)) {
        entry.compression = PakCompression::Zstd;
        if (entry.dictionary > header.dictionary_count) {
            return corrupt("pak: entry trỏ tới dictionary không có");
        }
    } else {
        return corrupt("pak: kiểu nén lạ", compression);
    }
    if (u64{entry.path_offset} + entry.path_length > strings.size() ||
        !is_valid_virtual_path(strings.substr(entry.path_offset, entry.path_length))) {
        return corrupt("pak: đường dẫn của entry sai");
    }
    return entry;
}

}  // namespace

Result<PakHeader> parse_pak_header(const std::span<const std::byte, kPakHeaderSize> header,
                                   const u64 file_size) noexcept {
    if (!std::ranges::equal(header.first<kPakMagic.size()>(), kPakMagic)) {
        return fail(ErrorCode::InvalidArgument, "pak: không phải tệp pak");
    }
    const std::span<const std::byte> bytes = header;
    const auto version = read_field<u16, 8>(bytes);
    if (version != kPakVersion) {
        return fail(ErrorCode::Unimplemented, "pak: phiên bản định dạng lạ", version);
    }
    if (read_field<u16, 10>(bytes) != 0 ||
        !std::ranges::all_of(bytes.subspan(40),
                             [](const std::byte b) { return b == std::byte{0}; })) {
        return corrupt("pak: flags hay vùng dành riêng khác 0");
    }
    PakHeader result;
    result.entry_count = read_field<u32, 12>(bytes);
    result.dictionary_count = read_field<u32, 16>(bytes);
    result.string_table_size = read_field<u32, 20>(bytes);
    result.index_offset = read_field<u64, 24>(bytes);
    result.index_size = read_field<u64, 32>(bytes);
    if (result.entry_count > kMaxPakEntries || result.dictionary_count > kMaxPakDictionaries) {
        return corrupt("pak: quá nhiều entry hay dictionary");
    }
    if (result.index_offset < kPakHeaderSize || result.index_offset > file_size ||
        result.index_size != file_size - result.index_offset ||
        result.index_size > kMaxPakIndexBytes) {
        return corrupt("pak: index không kéo tới đúng cuối tệp");
    }
    // Không tràn: entry_count, dictionary_count đã bị chặn, string_table_size là u32.
    const u64 fixed = (u64{result.entry_count} * kPakEntrySize) +
                      (u64{result.dictionary_count} * kPakDictionaryRecordSize) +
                      result.string_table_size;
    if (fixed > result.index_size) {
        return corrupt("pak: index ngắn hơn các bảng của nó");
    }
    return result;
}

Result<PakIndex> PakIndex::parse(const std::span<const std::byte, kPakHeaderSize> header,
                                 std::vector<std::byte> index, const u64 file_size) noexcept {
    const Result<PakHeader> parsed = parse_pak_header(header, file_size);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (index.size() != parsed->index_size) {
        return corrupt("pak: vùng index sai cỡ", static_cast<i64>(index.size()));
    }
    PakIndex result;
    result.index_ = std::move(index);
    const std::span<const std::byte> bytes = result.index_;
    const usize entries_size = usize{parsed->entry_count} * kPakEntrySize;
    const usize records_size = usize{parsed->dictionary_count} * kPakDictionaryRecordSize;
    result.strings_offset_ = entries_size + records_size;
    const std::string_view strings =
        core::as_chars(bytes.subspan(result.strings_offset_, parsed->string_table_size));
    const usize dictionary_data = result.strings_offset_ + parsed->string_table_size;

    result.entries_.reserve(parsed->entry_count);
    for (usize i = 0; i < parsed->entry_count; ++i) {
        const Result<PakEntry> entry =
            parse_entry(bytes.subspan(i * kPakEntrySize, kPakEntrySize), strings, *parsed);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        const std::string_view path = strings.substr(entry->path_offset, entry->path_length);
        if (!result.entries_.empty() && !(result.path(result.entries_.size() - 1) < path)) {
            return corrupt("pak: entry không xếp tăng dần theo đường dẫn", static_cast<i64>(i));
        }
        result.max_stored_size_ = std::max(result.max_stored_size_, entry->stored_size);
        result.entries_.push_back(*entry);
    }

    const usize data_size = bytes.size() - dictionary_data;
    result.dictionaries_.reserve(parsed->dictionary_count);
    for (usize k = 0; k < parsed->dictionary_count; ++k) {
        const std::span<const std::byte> record =
            bytes.subspan(entries_size + (k * kPakDictionaryRecordSize), kPakDictionaryRecordSize);
        const auto offset = read_field<u32, 0>(record);
        const auto size = read_field<u32, 4>(record);
        if (size == 0 || size > kMaxPakDictionaryBytes || offset > data_size ||
            size > data_size - offset) {
            return corrupt("pak: dictionary nằm ngoài vùng dữ liệu dictionary",
                           static_cast<i64>(k));
        }
        result.dictionaries_.push_back({dictionary_data + offset, size});
    }
    return result;
}

Result<PakIndex> PakIndex::parse_image(const std::span<const std::byte> image) noexcept {
    if (image.size() < kPakHeaderSize) {
        return fail(ErrorCode::InvalidArgument, "pak: nhỏ hơn header");
    }
    const std::span<const std::byte, kPakHeaderSize> header = image.first<kPakHeaderSize>();
    const Result<PakHeader> parsed = parse_pak_header(header, image.size());
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const std::span<const std::byte> index =
        image.subspan(static_cast<usize>(parsed->index_offset));
    return parse(header, std::vector<std::byte>(index.begin(), index.end()), image.size());
}

const PakEntry& PakIndex::entry(const usize i) const noexcept {
    ORION_ASSERT(i < entries_.size(), "entry {} ngoài khoảng {}", i, entries_.size());
    return entries_[i];
}

std::string_view PakIndex::path(const usize i) const noexcept {
    const PakEntry& e = entry(i);
    return core::as_chars(
        std::span<const std::byte>(index_).subspan(strings_offset_ + e.path_offset, e.path_length));
}

std::optional<usize> PakIndex::find(const std::string_view path) const noexcept {
    usize low = 0;
    usize high = entries_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        const std::string_view candidate = this->path(middle);
        if (candidate == path) {
            return middle;
        }
        if (candidate < path) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return std::nullopt;
}

std::span<const std::byte> PakIndex::dictionary(const usize k) const noexcept {
    ORION_ASSERT(k >= 1 && k <= dictionaries_.size(), "dictionary {} ngoài khoảng {}", k,
                 dictionaries_.size());
    const DictionaryRange range = dictionaries_[k - 1];
    return std::span<const std::byte>(index_).subspan(range.offset, range.size);
}

PakReadContext::PakReadContext() noexcept = default;
PakReadContext::PakReadContext(PakReadContext&& other) noexcept = default;
PakReadContext& PakReadContext::operator=(PakReadContext&& other) noexcept = default;
PakReadContext::~PakReadContext() = default;

void PakReadContext::reserve(const u64 stored_size) {
    if (scratch_.size() < stored_size) {
        scratch_.resize(static_cast<usize>(stored_size));
    }
}

PakReader::PakReader(File file, PakIndex index,
                     std::unique_ptr<detail::PakDictionaries> dictionaries) noexcept
    : file_(std::move(file)), index_(std::move(index)), dictionaries_(std::move(dictionaries)) {}

PakReader::PakReader(PakReader&& other) noexcept = default;
PakReader& PakReader::operator=(PakReader&& other) noexcept = default;
PakReader::~PakReader() = default;

Result<PakReader> PakReader::open(File file, const u64 expected_file_size,
                                  const crypto::Hash& expected_index_hash) noexcept {
    if (file.size() != expected_file_size) {
        return corrupt("pak: cỡ tệp khác manifest", static_cast<i64>(file.size()));
    }
    return open_checked(std::move(file), &expected_index_hash);
}

#if ORION_DEV_TOOLS
Result<PakReader> PakReader::open_unverified(File file) noexcept {
    return open_checked(std::move(file), nullptr);
}
#endif

Result<PakReader> PakReader::open_checked(File file, const crypto::Hash* expected) noexcept {
    if (file.size() < kPakHeaderSize) {
        return fail(ErrorCode::InvalidArgument, "pak: tệp nhỏ hơn header");
    }
    std::array<std::byte, kPakHeaderSize> header{};
    if (const Result<void> read = file.read_exact(0, header); !read) {
        return std::unexpected(read.error());
    }
    const Result<PakHeader> parsed = parse_pak_header(header, file.size());
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    std::vector<std::byte> index(static_cast<usize>(parsed->index_size));
    if (const Result<void> read = file.read_exact(parsed->index_offset, index); !read) {
        return std::unexpected(read.error());
    }
    if (expected != nullptr) {
        crypto::Hasher hasher;
        hasher.update(header);
        hasher.update(index);
        if (!crypto::equal_constant_time(hasher.finish(), *expected)) {
            return corrupt("pak: hash của index khác manifest");
        }
    }
    Result<PakIndex> parsed_index = PakIndex::parse(header, std::move(index), file.size());
    if (!parsed_index) {
        return std::unexpected(parsed_index.error());
    }
    auto dictionaries = std::make_unique<detail::PakDictionaries>();
    for (usize k = 1; k <= parsed_index->dictionary_count(); ++k) {
        const std::span<const std::byte> content = parsed_index->dictionary(k);
        ZSTD_DDict* dictionary = ZSTD_createDDict(content.data(), content.size());
        // NULL khi dictionary có magic của Zstd mà bảng entropy hỏng, hoặc khi hết bộ nhớ.
        if (dictionary == nullptr) {
            return corrupt("pak: không dựng được dictionary", static_cast<i64>(k));
        }
        dictionaries->dictionaries.emplace_back(dictionary);
    }
    return PakReader(std::move(file), std::move(*parsed_index), std::move(dictionaries));
}

Result<usize> PakReader::read(const usize i, const std::span<std::byte> out,
                              PakReadContext& context) const noexcept {
    if (i >= index_.entry_count()) {
        return fail(ErrorCode::InvalidArgument, "pak: entry ngoài khoảng", static_cast<i64>(i));
    }
    const PakEntry& entry = index_.entry(i);
    if (out.size() < entry.original_size) {
        return fail(ErrorCode::InvalidArgument, "pak: bộ đệm ra nhỏ hơn entry",
                    static_cast<i64>(entry.original_size));
    }
    const auto original_size = static_cast<usize>(entry.original_size);
    if (entry.compression == PakCompression::None) {
        const std::span<std::byte> target = out.first(original_size);
        if (const Result<void> read = file_.read_exact(entry.offset, target); !read) {
            return std::unexpected(read.error());
        }
        if (!crypto::equal_constant_time(crypto::hash(target), entry.hash)) {
            return corrupt("pak: hash của entry sai", static_cast<i64>(i));
        }
        return original_size;
    }

    context.reserve(entry.stored_size);
    const std::span<std::byte> blob =
        std::span(context.scratch_).first(static_cast<usize>(entry.stored_size));
    if (const Result<void> read = file_.read_exact(entry.offset, blob); !read) {
        return std::unexpected(read.error());
    }
    if (!crypto::equal_constant_time(crypto::hash(blob), entry.hash)) {
        return corrupt("pak: hash của entry sai", static_cast<i64>(i));
    }
    // Blob đã khớp index đã kiểm; các phép kiểm dưới là lớp phòng thủ thứ hai của ADR 0012.
    if (blob.size() < sizeof(u32) ||
        core::load_le<u32>(blob.first<sizeof(u32)>()) != kZstdFrameMagic ||
        ZSTD_getFrameContentSize(blob.data(), blob.size()) != entry.original_size ||
        ZSTD_findFrameCompressedSize(blob.data(), blob.size()) != blob.size()) {
        return corrupt("pak: frame Zstd sai", static_cast<i64>(i));
    }
    if (context.decoder_ == nullptr) {
        auto decoder = std::make_unique<detail::ZstdDecoder>();
        decoder->context = ZSTD_createDCtx();
        if (decoder->context == nullptr) {
            return fail(ErrorCode::ResourceExhausted, "pak: không tạo được bộ giải nén");
        }
        context.decoder_ = std::move(decoder);
    }
    ZSTD_DCtx* const decoder = context.decoder_->context;
    const usize written =
        entry.dictionary == 0
            ? ZSTD_decompressDCtx(decoder, out.data(), original_size, blob.data(), blob.size())
            : ZSTD_decompress_usingDDict(decoder, out.data(), original_size, blob.data(),
                                         blob.size(),
                                         dictionaries_->dictionaries[entry.dictionary - 1U].get());
    if (ZSTD_isError(written) != 0U || written != original_size) {
        return corrupt("pak: giải nén thất bại", static_cast<i64>(i));
    }
    return original_size;
}

}  // namespace orion::io
