// Tệp trên Windows: CreateFileW, ReadFile theo vị trí (OVERLAPPED trên handle đồng bộ), WriteFile,
// FlushFileBuffers. Đổi tên đè dùng ngữ nghĩa POSIX (FileRenameInfoEx) để thay được tệp đích đang
// mở, như rename(2); MoveFileExW không làm được (CI run 36328657911 đo thấy commit thất bại khi tệp
// đích còn handle đọc). Đường dẫn UTF-8 đổi sang UTF-16; đường dẫn dài được đổi sang dạng tuyệt đối
// có tiền tố "\\?\" để vượt giới hạn MAX_PATH.

#include "engine/io/detail/native_file.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// UTF-8 sang UTF-16 (File::open đã kiểm UTF-8).
[[nodiscard]] Result<std::wstring> utf16(const std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    if (text.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn quá dài");
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn không đổi được sang UTF-16");
    }
    std::wstring wide(static_cast<usize>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), wide.data(), length) != length) {
        return fail(ErrorCode::InvalidArgument, "io: đường dẫn không đổi được sang UTF-16");
    }
    return wide;
}

// Dạng tuyệt đối "\\?\" của một đường dẫn: GetFullPathNameW chuẩn hoá ('/' thành '\', ".", "..")
// rồi thêm "\\?\", hoặc "\\?\UNC\" cho đường dẫn mạng. Đường dẫn đã có "\\?\" và đường dẫn thiết bị
// "\\.\" giữ nguyên.
[[nodiscard]] Result<std::wstring> verbatim_path(std::wstring wide) {
    if (wide.starts_with(L"\\\\?\\")) {
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
    if (full.starts_with(L"\\\\.\\")) {
        return full;
    }
    if (full.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

// Đường dẫn ngắn giữ nguyên; từ kLongPathThreshold trở lên đổi sang dạng "\\?\".
[[nodiscard]] Result<std::wstring> wide_path(const char* path) {
    Result<std::wstring> wide = utf16(path);
    if (!wide || wide->size() < kLongPathThreshold) {
        return wide;
    }
    return verbatim_path(std::move(*wide));
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

namespace {

// Đổi tên đè theo ngữ nghĩa POSIX: thay được tệp đích kể cả khi nó đang mở với FILE_SHARE_DELETE,
// handle cũ vẫn đọc tệp cũ, như rename(2). Trả false khi hệ điều hành hay hệ thống tệp không nhận
// FileRenameInfoEx, để bên gọi lùi về MoveFileExW.
[[nodiscard]] Result<bool> posix_rename(const std::wstring& from, const std::wstring& to) {
    const HANDLE handle =
        CreateFileW(from.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return windows_error(GetLastError(), "io: không mở được tệp để đổi tên");
    }
    // FILE_RENAME_INFO kết thúc bằng mảng tên độ dài thay đổi; FileName[1] chừa sẵn chỗ cho ký tự
    // kết thúc. Kiểu C chỉ gồm dữ liệu nên được tạo ngầm trong vùng byte (C++20, P0593).
    const usize name_bytes = to.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
    void* const raw = storage.data();
    auto* const info = static_cast<FILE_RENAME_INFO*>(raw);
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(static_cast<void*>(&info->FileName[0]), to.data(), name_bytes);
    const BOOL renamed = SetFileInformationByHandle(handle, FileRenameInfoEx, info,
                                                    static_cast<DWORD>(storage.size()));
    const DWORD error = GetLastError();
    static_cast<void>(CloseHandle(handle));
    if (renamed != 0) {
        return true;
    }
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_FUNCTION) {
        return false;
    }
    return windows_error(error, "io: SetFileInformationByHandle thất bại");
}

}  // namespace

Result<void> replace_file(const char* from, const char* to) noexcept {
    const Result<std::wstring> wide_from = wide_path(from);
    if (!wide_from) {
        return std::unexpected(wide_from.error());
    }
    Result<std::wstring> relative_to = utf16(to);
    if (!relative_to) {
        return std::unexpected(relative_to.error());
    }
    // Đích luôn ở dạng tuyệt đối "\\?\", để không phụ thuộc cách FILE_RENAME_INFO hiểu đường dẫn
    // tương đối.
    const Result<std::wstring> wide_to = verbatim_path(std::move(*relative_to));
    if (!wide_to) {
        return std::unexpected(wide_to.error());
    }
    const Result<bool> renamed = posix_rename(*wide_from, *wide_to);
    if (!renamed) {
        return std::unexpected(renamed.error());
    }
    if (*renamed) {
        return {};
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
