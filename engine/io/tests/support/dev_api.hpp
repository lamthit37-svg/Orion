#pragma once

// Khái niệm mô tả phần API của engine/io chỉ có khi ORION_DEV_TOOLS, dùng ở hai phía:
// vfs_test.cpp kiểm ở DEV rằng chúng có, ship_api_check.cpp kiểm ở cấu hình ship rằng chúng không
// có. Phía DEV chứng minh mỗi khái niệm nhận ra được hàm của nó, để phía ship không đúng một cách
// vô nghĩa (ví dụ vì gõ sai tên hàm).

#include "engine/io/file.hpp"
#include "engine/io/manifest.hpp"
#include "engine/io/pak.hpp"

#include <string_view>
#include <utility>

namespace orion::io::testing {

// Đọc tệp rời.
template <class Vfs>
concept MountsDirectories =
    requires(Vfs& vfs, std::string_view directory) { vfs.mount_directory(directory); };

// Mount pak không qua manifest.
template <class Vfs>
concept MountsUnlistedPaks =
    requires(Vfs& vfs, PakReader reader) { vfs.mount_pak(std::move(reader)); };

// Mount pak của manifest đã ký: có ở mọi cấu hình.
template <class Vfs>
concept MountsManifests =
    requires(Vfs& vfs, const VerifiedManifest& manifest, std::string_view directory) {
        { vfs.mount_manifest(manifest, directory) };
    };

// Mở pak mà không so index_hash với manifest.
template <class Reader>
concept OpensUnverified = requires(File file) { Reader::open_unverified(std::move(file)); };

}  // namespace orion::io::testing
