// Tệp trên Windows: CreateFileW, ReadFile theo vị trí (OVERLAPPED trên handle đồng bộ), WriteFile,
// FlushFileBuffers, MoveFileExW. Đường dẫn UTF-8 đổi sang UTF-16; đường dẫn dài được đổi sang dạng
// tuyệt đối có tiền tố "\\?\" để vượt giới hạn MAX_PATH.

#include "engine/io/detail/native_file.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace orion::io::detail {
namespace {

// Một lần ReadFile hay WriteFile nhận cỡ DWORD; giữ mỗi lần dưới 1 GiB, vòng lặp lo phần còn lại.
constexpr usize kMaxTransfer = usize{1} << 30U;
// Từ ngưỡng này trở lên, đường dẫn được đổi sang dạng "\\?\" để không đụng giới hạn MAX_PATH (260);
// ngưỡng chừa lề 12 ký tự dưới giới hạn đó.
constexpr usize kLongPathThreshold = 248;

[[nodiscard]] std::unexpected<Error> windows_error(const DWORD error,
                                                   const ErrorContext context) noexcept {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_NETPATH:
            return fail(ErrorCode::NotFound, context, static_cast<i64>(error));
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return fail(ErrorCode::PermissionDenied, context, static_cast<i64>(error));
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return fail(ErrorCode::AlreadyExists, context, static_cast<i64>(error));
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return fail(ErrorCode::ResourceExhausted, context, static_cast<i64>(error));
        default:
            return fail(ErrorCode::Io, context, static_cast<i64>(error));
    }
}

[[nodiscard]] HANDLE handle_of(const NativeFile file) noexcept {
    return std::bit_cast<HANDLE>(file.handle);
}

[[nodiscard]] NativeFile from_handle(const HANDLE handle) noexcept {
    return NativeFile{std::bit_cast<std::intptr_t>(handle)};
}

// UTF-8 sang UTF-16 (File::open đã kiểm UTF-8). Đường dẫn dài được làm tuyệt đối bằng
// GetFullPathNameW rồi thêm "\\?\" (hoặc "\\?\UNC\" cho đường dẫn mạng).
[[nodiscard]] Result<std::wstring> wide_path(const char* path) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (length <= 0) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn không đổi được sang UTF-16");
    }
    std::wstring wide(static_cast<usize>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), length) !=
        length) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn không đổi được sang UTF-16");
    }
    wide.pop_back();
    if (wide.size() < kLongPathThreshold || wide.starts_with(L"\\\\?\\")) {
        return wide;
    }
    const DWORD needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return windows_error(GetLastError(), "io: GetFullPathNameW thất bại");
    }
    std::wstring full(needed, L'\0');
    const DWORD written = GetFullPathNameW(wide.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return windows_error(GetLastError(), "io: GetFullPathNameW thất bại");
    }
    full.resize(written);
    if (full.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

}  // namespace

Result<NativeFile> open_for_read(const char* path) noexcept {
    const Result<std::wstring> wide = wide_path(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    // FILE_SHARE_DELETE để AtomicFileWriter hay trình vá đổi tên đè được tệp đang mở đọc.
    const HANDLE handle =
        CreateFileW(wide->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return windows_error(GetLastError(), "io: không mở được tệp để đọc");
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        static_cast<void>(CloseHandle(handle));
        return fail(ErrorCode::InvalidArgument, "io: không phải tệp thường");
    }
    return from_handle(handle);
}

Result<NativeFile> create_for_write(const char* path) noexcept {
    const Result<std::wstring> wide = wide_path(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    const HANDLE handle = CreateFileW(wide->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return windows_error(GetLastError(), "io: không tạo được tệp để ghi");
    }
    return from_handle(handle);
}

void close_file(const NativeFile file) noexcept {
    static_cast<void>(CloseHandle(handle_of(file)));
}

Result<u64> file_size(const NativeFile file) noexcept {
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle_of(file), &size) == 0) {
        return windows_error(GetLastError(), "io: GetFileSizeEx thất bại");
    }
    return static_cast<u64>(size.QuadPart);
}

Result<usize> read_at(const NativeFile file, const u64 offset,
                      const std::span<std::byte> out) noexcept {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset & 0xFFFF'FFFFU);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    DWORD read = 0;
    const auto count = static_cast<DWORD>(std::min(out.size(), kMaxTransfer));
    if (ReadFile(handle_of(file), out.data(), count, &read, &position) == 0) {
        const DWORD error = GetLastError();
        // Đọc từ cuối tệp trở đi có thể báo ERROR_HANDLE_EOF thay vì trả 0 byte; cả hai đều là hết
        // tệp.
        if (error == ERROR_HANDLE_EOF) {
            return usize{0};
        }
        return windows_error(error, "io: ReadFile thất bại");
    }
    return static_cast<usize>(read);
}

Result<void> write_all(const NativeFile file, std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>(std::min(data.size(), kMaxTransfer));
        if (WriteFile(handle_of(file), data.data(), count, &written, nullptr) == 0) {
            return windows_error(GetLastError(), "io: WriteFile thất bại");
        }
        data = data.subspan(written);
    }
    return {};
}

Result<void> sync_file(const NativeFile file) noexcept {
    if (FlushFileBuffers(handle_of(file)) == 0) {
        return windows_error(GetLastError(), "io: FlushFileBuffers thất bại");
    }
    return {};
}

Result<void> replace_file(const char* from, const char* to) noexcept {
    const Result<std::wstring> wide_from = wide_path(from);
    if (!wide_from) {
        return std::unexpected(wide_from.error());
    }
    const Result<std::wstring> wide_to = wide_path(to);
    if (!wide_to) {
        return std::unexpected(wide_to.error());
    }
    if (MoveFileExW(wide_from->c_str(), wide_to->c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return windows_error(GetLastError(), "io: MoveFileExW thất bại");
    }
    return {};
}

Result<void> remove_file(const char* path) noexcept {
    const Result<std::wstring> wide = wide_path(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    if (DeleteFileW(wide->c_str()) == 0) {
        return windows_error(GetLastError(), "io: DeleteFileW thất bại");
    }
    return {};
}

}  // namespace orion::io::detail
