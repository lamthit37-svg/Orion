#pragma once

// Biến môi trường của tiến trình: cấu hình của server lúc chạy (chuỗi kết nối DB) và của test (thư
// mục golden, PostgreSQL của test).
//
// Không dùng thẳng std::getenv: MSVC đánh dấu nó không an toàn (C4996, thành lỗi dưới /WX), và trên
// Windows nó trả chuỗi theo code page ANSI chứ không phải UTF-8. Bản Windows (win/) đọc bằng
// GetEnvironmentVariableW rồi đổi sang UTF-8; bản POSIX (linux/, apple/) đọc bằng getenv.
//
// Luồng: gọi được từ mọi luồng, vì dự án không đổi biến môi trường (setenv, putenv) lúc đang chạy.

#include <optional>
#include <string>
#include <string_view>

namespace orion::core {

// Giá trị của biến `name` theo UTF-8; nullopt khi biến không có, khi tên rỗng hay có ký tự NUL, và
// (Windows) khi tên hay giá trị không đổi được giữa UTF-8 và UTF-16.
[[nodiscard]] std::optional<std::string> environment_variable(std::string_view name);

}  // namespace orion::core
