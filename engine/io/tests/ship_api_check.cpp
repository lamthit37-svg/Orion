// Luật của CLAUDE.md X.9 cho bản ship: không đọc tệp rời, chỉ mount pak có trong manifest đã ký, và
// kiểm hash khi mount. CI không build preset ship, nên tệp này được biên dịch với macro cấu hình
// của bản ship ở mọi preset (orion_apply_flags(... SHIP) trong engine/io/CMakeLists.txt); chỉ biên
// dịch, không link. vfs_test.cpp kiểm chiều ngược lại ở DEV.

#include "engine/io/manifest.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/tests/support/dev_api.hpp"
#include "engine/io/vfs.hpp"

#include <type_traits>

#if !ORION_SHIP || ORION_DEV_TOOLS
#error "tệp này chỉ biên dịch với cấu hình ship"
#endif

namespace orion::io::testing {

static_assert(!MountsDirectories<Vfs>, "bản ship không đọc tệp rời");
static_assert(!MountsUnlistedPaks<Vfs>, "bản ship chỉ mount pak của manifest đã ký");
static_assert(!OpensUnverified<PakReader>, "bản ship không mở pak mà không so index_hash");
static_assert(MountsManifests<Vfs>);
// VerifiedManifest chỉ dựng được qua VerifiedManifest::verify.
static_assert(!std::is_default_constructible_v<VerifiedManifest>);

}  // namespace orion::io::testing
