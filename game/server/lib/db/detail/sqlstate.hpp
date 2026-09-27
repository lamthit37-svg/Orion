#pragma once

// SQLSTATE của PostgreSQL thành Error (bảng ở sqlstate.cpp, theo phụ lục "PostgreSQL Error Codes"
// của tài liệu PostgreSQL).

#include "engine/core/error.hpp"

#include <string_view>

namespace orion::db::detail {

// Error của một SQLSTATE năm ký tự: mã theo mã cụ thể nếu có trong bảng, không thì theo lớp (hai
// ký tự đầu), không nữa thì Internal. detail() là sqlstate_code(state). Chuỗi không đúng năm ký tự
// cũng thành Internal: nó đến từ server, không phải từ code gọi (CLAUDE.md X.5).
[[nodiscard]] Error sqlstate_error(std::string_view state) noexcept;

}  // namespace orion::db::detail
