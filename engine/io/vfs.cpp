#include "engine/io/vfs.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/file.hpp"
#include "engine/io/manifest.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/path.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

// `directory` nối `name` bằng đúng một dấu ngăn.
[[nodiscard]] std::string join(const std::string_view directory, const std::string_view name) {
    std::string path(directory);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += name;
    return path;
}

}  // namespace

Result<void> Vfs::mount_manifest(const VerifiedManifest& manifest,
                                 const std::string_view data_directory) {
    std::vector<Mount> opened;
    opened.reserve(manifest.paks().size());
    for (const ManifestPak& pak : manifest.paks()) {
        Result<File> file = File::open(join(data_directory, pak_file_name(pak.file_hash)));
        if (!file) {
            return std::unexpected(file.error());
        }
        Result<PakReader> reader = PakReader::open(std::move(*file), pak.file_size, pak.index_hash);
        if (!reader) {
            return std::unexpected(reader.error());
        }
        Mount mount;
        mount.pak.emplace(std::move(*reader));
        opened.push_back(std::move(mount));
    }
    mounts_.reserve(mounts_.size() + opened.size());
    for (Mount& mount : opened) {
        mounts_.push_back(std::move(mount));
    }
    return {};
}

#if ORION_DEV_TOOLS
void Vfs::mount_directory(const std::string_view directory) {
    Mount mount;
    mount.directory = join(directory, "");
    mounts_.push_back(std::move(mount));
}

void Vfs::mount_pak(PakReader reader) {
    Mount mount;
    mount.pak.emplace(std::move(reader));
    mounts_.push_back(std::move(mount));
}
#endif

Result<Vfs::Location> Vfs::locate(const VirtualPath& path) const noexcept {
    for (const Mount& mount : std::views::reverse(mounts_)) {
        if (mount.pak) {
            const std::optional<usize> entry = mount.pak->index().find(path.view());
            if (entry) {
                Location location;
                location.pak = &*mount.pak;
                location.entry = *entry;
                location.size = mount.pak->index().entry(*entry).original_size;
                return location;
            }
            continue;
        }
#if ORION_DEV_TOOLS
        Result<File> file = File::open(mount.directory + std::string(path.view()));
        if (file) {
            Location location;
            location.size = file->size();
            location.file.emplace(std::move(*file));
            return location;
        }
        if (file.error().code() != ErrorCode::NotFound) {
            return std::unexpected(file.error());
        }
#endif
    }
    return fail(ErrorCode::NotFound, "vfs: không mount nào có tệp này");
}

Result<usize> Vfs::read_located(const Location& location, const std::span<std::byte> out,
                                PakReadContext& context) noexcept {
    if (out.size() < location.size) {
        return fail(ErrorCode::InvalidArgument, "vfs: bộ đệm ra nhỏ hơn tệp",
                    static_cast<i64>(location.size));
    }
#if ORION_DEV_TOOLS
    if (location.pak == nullptr) {
        // locate trả hoặc một entry của pak, hoặc một tệp rời đã mở.
        ORION_VERIFY(location.file.has_value(), "vfs: nơi chứa rỗng");
        const auto size = static_cast<usize>(location.size);
        if (const Result<void> read = location.file->read_exact(0, out.first(size)); !read) {
            return std::unexpected(read.error());
        }
        return size;
    }
#endif
    ORION_VERIFY(location.pak != nullptr, "vfs: nơi chứa rỗng");
    return location.pak->read(location.entry, out, context);
}

Result<u64> Vfs::size(const VirtualPath& path) const noexcept {
    const Result<Location> location = locate(path);
    if (!location) {
        return std::unexpected(location.error());
    }
    return location->size;
}

Result<usize> Vfs::read(const VirtualPath& path, const std::span<std::byte> out,
                        PakReadContext& context) const noexcept {
    const Result<Location> location = locate(path);
    if (!location) {
        return std::unexpected(location.error());
    }
    return read_located(*location, out, context);
}

Result<std::vector<std::byte>> Vfs::read_all(const VirtualPath& path,
                                             PakReadContext& context) const noexcept {
    const Result<Location> location = locate(path);
    if (!location) {
        return std::unexpected(location.error());
    }
    std::vector<std::byte> content(static_cast<usize>(location->size));
    const Result<usize> read = read_located(*location, content, context);
    if (!read) {
        return std::unexpected(read.error());
    }
    return content;
}

}  // namespace orion::io
