#pragma once

// Tệp bí mật của tiến trình server (nội bộ; docs/formats/service.md, mục "Khoá và bí mật"): khoá,
// chuỗi kết nối DB.

#include "engine/core/error.hpp"
#include "game/server/lib/service/service.hpp"

#include <string>
#include <string_view>

namespace orion::service::detail {

// true khi tên tệp (phần sau dấu '/' hay '\' cuối) có ".dev.": bí mật chỉ cho máy dev.
[[nodiscard]] bool is_dev_secret(std::string_view path) noexcept;

// Bí mật dev chỉ nạp được ở Environment::Local và không bao giờ trong bản ship. Lỗi:
// FailedPrecondition.
[[nodiscard]] Result<void> allow_secret(std::string_view path, Environment environment) noexcept;

// Đọc một tệp bí mật (tối đa kMaxSecretFileBytes), bỏ một dòng mới ở cuối ("\n" hay "\r\n"). Lỗi:
// như allow_secret, như read_file, InvalidArgument khi tệp rỗng.
[[nodiscard]] Result<std::string> read_secret_file(std::string_view path, Environment environment);

// Ghi 0 lên nội dung của `text` rồi làm nó rỗng.
void wipe(std::string& text) noexcept;

}  // namespace orion::service::detail
