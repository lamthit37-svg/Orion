#include "engine/io/file.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"
#include "engine/io/detail/native_file.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace orion::io {
namespace {

// Tệp tạm trùng tên (writer khác đang ghi, hoặc tệp sót lại sau khi tiến trình chết) thì thử tên
// kế.
constexpr u32 kTemporaryAttempts = 16;

// API của hệ điều hành đọc chuỗi tới '\0' đầu tiên: đường dẫn chứa '\0' ở giữa sẽ bị cắt và trỏ
// nhầm sang tệp khác, nên bị từ chối.
[[nodiscard]] Result<std::string> native_path(const std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos || !core::is_valid_utf8(path)) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn rỗng, chứa '\\0' hay sai UTF-8");
    }
    return std::string(path);
}

}  // namespace

Result<File> File::open(const std::string_view path) noexcept {
    const Result<std::string> native_name = native_path(path);
    if (!native_name) {
        return std::unexpected(native_name.error());
    }
    const Result<detail::NativeFile> native = detail::open_for_read(native_name->c_str());
    if (!native) {
        return std::unexpected(native.error());
    }
    const Result<u64> size = detail::file_size(*native);
    if (!size) {
        detail::close_file(*native);
        return std::unexpected(size.error());
    }
    return File(*native, *size);
}

File::File(File&& other) noexcept
    : native_(std::exchange(other.native_, detail::NativeFile{})), size_(other.size_) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (native_.handle != -1) {
            detail::close_file(native_);
        }
        native_ = std::exchange(other.native_, detail::NativeFile{});
        size_ = other.size_;
    }
    return *this;
}

File::~File() {
    if (native_.handle != -1) {
        detail::close_file(native_);
    }
}

Result<void> File::read_exact(const u64 offset, const std::span<std::byte> out) const noexcept {
    if (offset > size_ || out.size() > size_ - offset) {
        return fail(ErrorCode::OutOfRange, "io: đọc quá cỡ tệp", static_cast<i64>(offset));
    }
    usize done = 0;
    while (done < out.size()) {
        const Result<usize> read = detail::read_at(native_, offset + done, out.subspan(done));
        if (!read) {
            return std::unexpected(read.error());
        }
        if (*read == 0) {
            return fail(ErrorCode::DataLoss, "io: tệp ngắn đi sau khi mở",
                        static_cast<i64>(offset + done));
        }
        done += *read;
    }
    return {};
}

Result<AtomicFileWriter> AtomicFileWriter::create(const std::string_view path) noexcept {
    Result<std::string> target = native_path(path);
    if (!target) {
        return std::unexpected(target.error());
    }
    for (u32 attempt = 0; attempt < kTemporaryAttempts; ++attempt) {
        std::string temporary = *target + ".tmp" + std::to_string(attempt);
        const Result<detail::NativeFile> native = detail::create_for_write(temporary.c_str());
        if (native) {
            return AtomicFileWriter(*native, std::move(temporary), std::move(*target));
        }
        if (native.error().code() != ErrorCode::AlreadyExists) {
            return std::unexpected(native.error());
        }
    }
    return fail(ErrorCode::AlreadyExists, "io: mọi tên tệp tạm đều đã có",
                static_cast<i64>(kTemporaryAttempts));
}

AtomicFileWriter::AtomicFileWriter(const detail::NativeFile native, std::string temporary,
                                   std::string target) noexcept
    : native_(native), temporary_(std::move(temporary)), target_(std::move(target)) {}

AtomicFileWriter::AtomicFileWriter(AtomicFileWriter&& other) noexcept
    : native_(std::exchange(other.native_, detail::NativeFile{})),
      temporary_(std::move(other.temporary_)),
      target_(std::move(other.target_)) {
    other.temporary_.clear();
    other.target_.clear();
}

AtomicFileWriter& AtomicFileWriter::operator=(AtomicFileWriter&& other) noexcept {
    if (this != &other) {
        discard();
        native_ = std::exchange(other.native_, detail::NativeFile{});
        temporary_ = std::move(other.temporary_);
        target_ = std::move(other.target_);
        other.temporary_.clear();
        other.target_.clear();
    }
    return *this;
}

AtomicFileWriter::~AtomicFileWriter() {
    discard();
}

void AtomicFileWriter::discard() noexcept {
    if (native_.handle == -1) {
        return;
    }
    detail::close_file(std::exchange(native_, detail::NativeFile{}));
    // Tệp tạm không xoá được (thư mục mất quyền ghi) chỉ còn là rác; tệp đích không bị chạm tới.
    static_cast<void>(detail::remove_file(temporary_.c_str()).has_value());
    temporary_.clear();
    target_.clear();
}

Result<void> AtomicFileWriter::write(const std::span<const std::byte> data) noexcept {
    if (native_.handle == -1) {
        return fail(ErrorCode::FailedPrecondition, "io: writer đã commit hoặc đã bị move");
    }
    return detail::write_all(native_, data);
}

Result<void> AtomicFileWriter::commit() noexcept {
    if (native_.handle == -1) {
        return fail(ErrorCode::FailedPrecondition, "io: writer đã commit hoặc đã bị move");
    }
    if (const Result<void> synced = detail::sync_file(native_); !synced) {
        discard();
        return synced;
    }
    detail::close_file(std::exchange(native_, detail::NativeFile{}));
    const Result<void> replaced = detail::replace_file(temporary_.c_str(), target_.c_str());
    if (!replaced) {
        static_cast<void>(detail::remove_file(temporary_.c_str()).has_value());
    }
    temporary_.clear();
    target_.clear();
    return replaced;
}

}  // namespace orion::io
