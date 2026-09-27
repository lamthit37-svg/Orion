// Tệp trên Linux và Android (bionic): open, pread, fstat, fsync, rename của POSIX. Lời gọi bị ngắt
// bởi tín hiệu (EINTR) được gọi lại.

#include "engine/io/detail/native_file.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <span>

namespace orion::io::detail {
namespace {

// Một lần gọi đọc hay ghi tối đa chừng này byte; Linux vốn cắt ở 0x7ffff000. Vòng lặp của bên gọi
// lo phần còn lại.
constexpr usize kMaxTransfer = usize{1} << 30U;

[[nodiscard]] std::unexpected<Error> errno_error(const int error,
                                                 const ErrorContext context) noexcept {
    switch (error) {
        case ENOENT:
        case ENOTDIR:
            return fail(ErrorCode::NotFound, context, error);
        case EACCES:
        case EPERM:
        case EROFS:
            return fail(ErrorCode::PermissionDenied, context, error);
        case EEXIST:
            return fail(ErrorCode::AlreadyExists, context, error);
        case ENOSPC:
            return fail(ErrorCode::ResourceExhausted, context, error);
        default:
            return fail(ErrorCode::Io, context, error);
    }
}

[[nodiscard]] int descriptor(const NativeFile file) noexcept {
    return static_cast<int>(file.handle);
}

// Gọi lại lời gọi hệ thống bị tín hiệu ngắt (EINTR); trả kết quả đầu tiên không phải EINTR.
template <class Call>
[[nodiscard]] auto retry_interrupted(const Call call) noexcept {
    while (true) {
        const auto result = call();
        if (result >= 0 || errno != EINTR) {
            return result;
        }
    }
}

}  // namespace

Result<NativeFile> open_for_read(const char* path) noexcept {
    const int fd = retry_interrupted([path] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) là API variadic của POSIX.
        return ::open(path, O_RDONLY | O_CLOEXEC);
    });
    if (fd < 0) {
        return errno_error(errno, "io: không mở được tệp để đọc");
    }
    // open(2) mở được cả thư mục; chỉ nhận tệp thường.
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return fail(ErrorCode::InvalidArgument, "io: không phải tệp thường");
    }
    return NativeFile{fd};
}

Result<NativeFile> create_for_write(const char* path) noexcept {
    const int fd = retry_interrupted([path] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) là API variadic của POSIX.
        return ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    });
    if (fd < 0) {
        return errno_error(errno, "io: không tạo được tệp để ghi");
    }
    return NativeFile{fd};
}

void close_file(const NativeFile file) noexcept {
    // Linux đóng descriptor kể cả khi close(2) trả EINTR, nên không gọi lại.
    ::close(descriptor(file));
}

Result<u64> file_size(const NativeFile file) noexcept {
    struct stat info{};
    if (::fstat(descriptor(file), &info) != 0) {
        return errno_error(errno, "io: fstat thất bại");
    }
    return static_cast<u64>(info.st_size);
}

Result<usize> read_at(const NativeFile file, const u64 offset,
                      const std::span<std::byte> out) noexcept {
    // Vị trí lớn hơn INT64_MAX thành off_t âm, và pread trả EINVAL (đã đo trên Linux x64).
    const usize count = std::min(out.size(), kMaxTransfer);
    const ssize_t read = retry_interrupted(
        [&] { return ::pread(descriptor(file), out.data(), count, static_cast<off_t>(offset)); });
    if (read < 0) {
        return errno_error(errno, "io: pread thất bại");
    }
    return static_cast<usize>(read);
}

Result<void> write_all(const NativeFile file, std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
        const ssize_t written =
            ::write(descriptor(file), data.data(), std::min(data.size(), kMaxTransfer));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_error(errno, "io: write thất bại");
        }
        data = data.subspan(static_cast<usize>(written));
    }
    return {};
}

Result<void> sync_file(const NativeFile file) noexcept {
    const int result = retry_interrupted([file] { return ::fsync(descriptor(file)); });
    if (result != 0) {
        return errno_error(errno, "io: fsync thất bại");
    }
    return {};
}

Result<void> replace_file(const char* from, const char* to) noexcept {
    if (std::rename(from, to) != 0) {
        return errno_error(errno, "io: rename thất bại");
    }
    return {};
}

Result<void> remove_file(const char* path) noexcept {
    if (::unlink(path) != 0) {
        return errno_error(errno, "io: unlink thất bại");
    }
    return {};
}

}  // namespace orion::io::detail
