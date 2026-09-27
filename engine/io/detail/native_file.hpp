#pragma once

// Lời gọi hệ điều hành cho tệp, cài ở win/, linux/, apple/ (ARCH §2). Đường dẫn là UTF-8, kết thúc
// bằng '\0'. Mọi hàm trả lỗi thay vì assert: tệp và đường dẫn là dữ liệu từ ngoài (X.5).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace orion::io::detail {

// fd của POSIX, hoặc HANDLE của Windows đổi sang số nguyên bằng std::bit_cast. -1 là "không có".
struct NativeFile {
    std::intptr_t handle = -1;
};

[[nodiscard]] Result<NativeFile> open_for_read(const char* path) noexcept;
// Tạo tệp mới để ghi; tệp đã có thì trả AlreadyExists (không bao giờ ghi đè ngầm).
[[nodiscard]] Result<NativeFile> create_for_write(const char* path) noexcept;
void close_file(NativeFile file) noexcept;

[[nodiscard]] Result<u64> file_size(NativeFile file) noexcept;
// Đọc tối đa out.size() byte từ offset mà không đổi vị trí dùng chung của tệp; trả số byte đọc
// được, 0 ở cuối tệp.
[[nodiscard]] Result<usize> read_at(NativeFile file, u64 offset, std::span<std::byte> out) noexcept;
[[nodiscard]] Result<void> write_all(NativeFile file, std::span<const std::byte> data) noexcept;
// Đẩy dữ liệu và metadata của tệp xuống thiết bị lưu trữ.
[[nodiscard]] Result<void> sync_file(NativeFile file) noexcept;

// Đổi tên `from` thành `to`, thay `to` nếu nó đã có, trong một bước với người đọc khác.
[[nodiscard]] Result<void> replace_file(const char* from, const char* to) noexcept;
[[nodiscard]] Result<void> remove_file(const char* path) noexcept;

}  // namespace orion::io::detail
