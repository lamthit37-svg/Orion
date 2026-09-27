// Pak hạt giống của fuzz target io_pak_index được sinh bằng tools/gen_pak_seeds.py, bộ ghi Python
// viết độc lập theo docs/formats/pak.md (hashlib.blake2b, struct). PakIndex phải đọc chúng đúng như
// đặc tả nói: đây là phép kiểm chéo giữa tài liệu định dạng và code.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/file.hpp"
#include "engine/io/pak.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace orion::io {
namespace {

[[nodiscard]] std::vector<std::byte> read_seed(const std::string_view name) {
    const std::string path = std::string(ORION_PAK_SEEDS_DIR) + "/" + std::string(name);
    const Result<File> file = File::open(path);
    EXPECT_TRUE(file.has_value()) << path;
    if (!file) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<usize>(file->size()));
    EXPECT_TRUE(file->read_exact(0, bytes).has_value());
    return bytes;
}

TEST(PakSeeds, IndependentWriterOutputParses) {
    struct Expected {
        std::string_view name;
        usize entries;
        usize dictionaries;
    };
    for (const Expected expected :
         {Expected{"empty", 0, 0}, Expected{"one_stored", 1, 0}, Expected{"nested_stored", 3, 0},
          Expected{"zstd_frames", 2, 0}, Expected{"dictionaries", 2, 2}, Expected{"gaps", 2, 0}}) {
        SCOPED_TRACE(expected.name);
        const Result<PakIndex> index = PakIndex::parse_image(read_seed(expected.name));
        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(index->entry_count(), expected.entries);
        EXPECT_EQ(index->dictionary_count(), expected.dictionaries);
    }
    const Result<PakIndex> nested = PakIndex::parse_image(read_seed("nested_stored"));
    ASSERT_TRUE(nested.has_value());
    EXPECT_EQ(nested->path(0), "data/items.bin");
    EXPECT_EQ(nested->path(2), "textures/stone_01.ktx2");
}

TEST(PakSeeds, IndependentWriterRejectsFollowTheErrorTable) {
    const Result<PakIndex> newer = PakIndex::parse_image(read_seed("version_2"));
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code(), ErrorCode::Unimplemented);
    const Result<PakIndex> truncated = PakIndex::parse_image(read_seed("truncated_header"));
    ASSERT_FALSE(truncated.has_value());
    EXPECT_EQ(truncated.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace orion::io
