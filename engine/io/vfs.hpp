#pragma once

// Hệ thống tệp ảo (ARCH §1, §2): đọc tệp theo đường dẫn ảo từ các nguồn đã mount.
//
// - Bản ship chỉ mount được qua mount_manifest: mỗi pak phải nằm trong một VerifiedManifest, và cỡ
//   tệp cùng index_hash được kiểm khi mount (CLAUDE.md X.9). Đọc tệp rời và mount pak không qua
//   manifest chỉ có khi ORION_DEV_TOOLS; tests/ship_api_check.cpp giữ luật này ở mọi preset.
// - DEV và CHẠY mount thư mục cooked dạng tệp rời; mỗi lần đọc mở lại tệp, nên hot reload không cần
//   gì thêm.
// - Mount theo thứ tự; khi tìm, mount sau che mount trước.
// - Mount ở một luồng, rồi đọc từ nhiều luồng cùng lúc: size, read và read_all là const, mỗi luồng
//   một PakReadContext. Không mount thêm khi còn luồng đang đọc.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/manifest.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/path.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orion::io {

class Vfs {
public:
    Vfs() noexcept = default;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;
    Vfs(Vfs&& other) noexcept = default;
    Vfs& operator=(Vfs&& other) noexcept = default;
    ~Vfs() = default;

    // Mount mọi pak của `manifest` theo thứ tự của nó, từ `data_directory`/<pak_file_name>. Mỗi pak
    // được mở bằng PakReader::open với cỡ tệp và index_hash của manifest. Lỗi của File::open hay
    // PakReader::open; khi lỗi, không pak nào của lần gọi này được mount.
    [[nodiscard]] Result<void> mount_manifest(const VerifiedManifest& manifest,
                                              std::string_view data_directory);

#if ORION_DEV_TOOLS
    // Thư mục cooked dạng tệp rời: đường dẫn ảo p đọc từ `directory`/p. Không kiểm thư mục lúc
    // mount; tệp không có thì tìm tiếp ở mount trước như mọi nguồn khác.
    void mount_directory(std::string_view directory);
    // Một pak đã mở sẵn, không qua manifest (tool, test).
    void mount_pak(PakReader reader);
#endif

    [[nodiscard]] usize mount_count() const noexcept { return mounts_.size(); }

    // Cỡ của tệp. Lỗi: NotFound khi không mount nào có nó; lỗi của File::open khi đường dẫn trong
    // một thư mục tệp rời có mà không mở được.
    [[nodiscard]] Result<u64> size(const VirtualPath& path) const noexcept;
    // Đọc cả tệp vào đầu `out` và trả cỡ của nó. Lỗi như size, cộng InvalidArgument khi `out` nhỏ
    // hơn tệp, và lỗi của PakReader::read hay File::read_exact.
    [[nodiscard]] Result<usize> read(const VirtualPath& path, std::span<std::byte> out,
                                     PakReadContext& context) const noexcept;
    // Như read, vào một vector mới vừa đúng cỡ tệp.
    [[nodiscard]] Result<std::vector<std::byte>> read_all(const VirtualPath& path,
                                                          PakReadContext& context) const noexcept;

private:
    struct Mount {
        std::optional<PakReader> pak;
#if ORION_DEV_TOOLS
        // Thư mục tệp rời, có dấu ngăn ở cuối; dùng khi pak rỗng.
        std::string directory;
#endif
    };

    // Nơi chứa một tệp: entry của một pak, hoặc (DEV) tệp rời đã mở.
    struct Location {
        const PakReader* pak = nullptr;
        usize entry = 0;
#if ORION_DEV_TOOLS
        std::optional<File> file;
#endif
        u64 size = 0;
    };

    [[nodiscard]] Result<Location> locate(const VirtualPath& path) const noexcept;
    [[nodiscard]] static Result<usize> read_located(const Location& location,
                                                    std::span<std::byte> out,
                                                    PakReadContext& context) noexcept;

    std::vector<Mount> mounts_;
};

}  // namespace orion::io
