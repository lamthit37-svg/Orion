#pragma once

// Tệp của hệ điều hành cho engine/io (ARCH §4.1): đọc theo vị trí và ghi nguyên tử. Đường dẫn là
// đường dẫn gốc của hệ điều hành, UTF-8; nội dung tệp là dữ liệu từ ngoài, nên mọi lỗi là Result.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/detail/native_file.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace orion::io {

// Tệp chỉ đọc, đọc theo vị trí: không có con trỏ vị trí dùng chung, nên nhiều luồng gọi read_exact
// trên cùng một File cùng lúc được. Cỡ tệp đọc một lần lúc mở.
class File {
public:
    // Lỗi: NotFound, PermissionDenied, InvalidArgument (đường dẫn không phải UTF-8 hợp lệ hay chứa
    // '\0'), Io.
    [[nodiscard]] static Result<File> open(std::string_view path) noexcept;

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    ~File();

    [[nodiscard]] u64 size() const noexcept { return size_; }

    // Đọc đúng out.size() byte bắt đầu ở offset. Vượt cỡ tệp lúc mở: OutOfRange; tệp ngắn đi sau
    // khi mở: DataLoss; hệ điều hành báo lỗi: Io.
    [[nodiscard]] Result<void> read_exact(u64 offset, std::span<std::byte> out) const noexcept;

private:
    File(detail::NativeFile native, u64 size) noexcept : native_(native), size_(size) {}

    detail::NativeFile native_;
    u64 size_ = 0;
};

// Ghi một tệp theo cách nguyên tử: ghi vào tệp tạm cạnh đích, đẩy xuống đĩa, rồi đổi tên đè lên
// đích. Người đọc chỉ thấy tệp cũ hoặc tệp mới đầy đủ; tiến trình chết giữa chừng chỉ để lại tệp
// tạm, không bao giờ để lại tệp đích ghi dở. Huỷ mà chưa commit thì xoá tệp tạm.
class AtomicFileWriter {
public:
    // Thư mục chứa `path` phải có sẵn. Lỗi như File::open, cộng AlreadyExists khi tên tệp tạm
    // trùng.
    [[nodiscard]] static Result<AtomicFileWriter> create(std::string_view path) noexcept;

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;
    AtomicFileWriter(AtomicFileWriter&& other) noexcept;
    AtomicFileWriter& operator=(AtomicFileWriter&& other) noexcept;
    ~AtomicFileWriter();

    [[nodiscard]] Result<void> write(std::span<const std::byte> data) noexcept;
    // Đẩy xuống đĩa rồi đổi tên; sau đó writer rỗng. Gọi trên writer rỗng: FailedPrecondition.
    [[nodiscard]] Result<void> commit() noexcept;

private:
    AtomicFileWriter(detail::NativeFile native, std::string temporary, std::string target) noexcept;
    void discard() noexcept;

    detail::NativeFile native_;
    std::string temporary_;
    std::string target_;
};

}  // namespace orion::io
