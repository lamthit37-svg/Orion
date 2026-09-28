#include "game/server/lib/service/service.hpp"

#include "engine/core/error.hpp"
#include "engine/core/narrow.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/io/file.hpp"
#include "game/server/lib/http/json.hpp"

#include <algorithm>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace orion::service {
namespace {

[[nodiscard]] bool is_ascii(const std::string_view text) noexcept {
    return std::ranges::all_of(text, [](const char c) { return static_cast<u8>(c) < 0x80U; });
}

// Cùng một luật trên mọi nền tảng, để một tệp cấu hình đọc giống nhau ở Linux lẫn Windows: bắt đầu
// bằng '/', '\\' hay "<chữ cái>:".
[[nodiscard]] bool is_absolute(const std::string_view path) noexcept {
    const bool drive = path.size() >= 2 && path[1] == ':' &&
                       ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
    return path.starts_with('/') || path.starts_with('\\') || drive;
}

}  // namespace

Result<Arguments> parse_arguments(const std::span<const char* const> argv) {
    // Đúng một cặp "--config <tệp>" sau tên chương trình.
    if (argv.size() != 3 || std::string_view(argv[1]) != "--config") {
        return fail(ErrorCode::InvalidArgument, "service: cách dùng: --config <tệp>");
    }
    Arguments out{.config_path = argv[2]};
    if (out.config_path.empty()) {
        return fail(ErrorCode::InvalidArgument, "service: đường dẫn cấu hình rỗng");
    }
    // argv của Windows theo code page ANSI, không phải UTF-8 mà engine/io đòi: chỉ nhận ASCII để
    // hai nền tảng đọc cùng một tệp.
    if (!is_ascii(out.config_path)) {
        return fail(ErrorCode::InvalidArgument, "service: đường dẫn cấu hình phải là ASCII");
    }
    return out;
}

Result<std::string> read_file(const std::string_view path, const usize max_bytes) {
    Result<io::File> file = io::File::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    if (file->size() > max_bytes) {
        return fail(ErrorCode::OutOfRange, "service: tệp lớn hơn giới hạn",
                    narrow<i64>(file->size()));
    }
    std::string text(narrow<usize>(file->size()), '\0');
    if (Result<void> read = file->read_exact(0, std::as_writable_bytes(std::span(text))); !read) {
        return std::unexpected(read.error());
    }
    return text;
}

ConfigFile::ConfigFile(std::string directory, http::json::Document document,
                       const http::json::Object root) noexcept
    : directory_(std::move(directory)), document_(std::move(document)), root_(root) {}

Result<ConfigFile> ConfigFile::load(const std::string_view path) {
    const Result<std::string> text = read_file(path, kMaxConfigBytes);
    if (!text) {
        return std::unexpected(text.error());
    }
    http::json::Document document;
    const Result<http::json::Value> root = document.parse(*text);
    if (!root) {
        return std::unexpected(root.error());
    }
    const Result<http::json::Object> object = root->object();
    if (!object) {
        return fail(ErrorCode::InvalidArgument, "config: gốc của tệp cấu hình phải là object");
    }
    const usize slash = path.find_last_of("/\\");
    std::string directory(slash == std::string_view::npos ? "" : path.substr(0, slash));
    // Move Document không làm cũ `object`: cây nằm trên heap (json.hpp).
    return ConfigFile(std::move(directory), std::move(document), *object);
}

std::string ConfigFile::resolve(const std::string_view relative) const {
    if (is_absolute(relative) || directory_.empty()) {
        return std::string(relative);
    }
    std::string path = directory_;
    path += '/';
    path += relative;
    return path;
}

namespace config {

Result<i64> integer(const http::json::Object object, const std::string_view key, const i64 min,
                    const i64 max, const i64 fallback, const ErrorContext context) {
    const std::optional<http::json::Value> value = object.find(key);
    if (!value) {
        return fallback;
    }
    const Result<i64> number = value->integer(min, max);
    if (!number) {
        return fail(ErrorCode::InvalidArgument, context);
    }
    return *number;
}

Result<i64> required_integer(const http::json::Object object, const std::string_view key,
                             const i64 min, const i64 max, const ErrorContext context) {
    if (!object.find(key)) {
        return fail(ErrorCode::InvalidArgument, context);
    }
    return integer(object, key, min, max, min, context);
}

Result<core::Duration> milliseconds(const http::json::Object object, const std::string_view key,
                                    const i64 min_ms, const i64 max_ms,
                                    const core::Duration fallback, const ErrorContext context) {
    return integer(object, key, min_ms, max_ms, fallback.as_milliseconds(), context)
        .transform([](const i64 ms) { return core::Duration::milliseconds(ms); });
}

Result<std::string> text(const http::json::Object object, const std::string_view key,
                         const std::string_view fallback, const usize max_bytes,
                         const ErrorContext context) {
    const std::optional<http::json::Value> value = object.find(key);
    if (!value) {
        return std::string(fallback);
    }
    const Result<std::string_view> found = value->string(max_bytes);
    if (!found) {
        return fail(ErrorCode::InvalidArgument, context);
    }
    return std::string(*found);
}

Result<std::string> required_text(const http::json::Object object, const std::string_view key,
                                  const usize max_bytes, const ErrorContext context) {
    if (!object.find(key)) {
        return fail(ErrorCode::InvalidArgument, context);
    }
    return text(object, key, {}, max_bytes, context);
}

Result<std::optional<http::json::Object>> section(const http::json::Object object,
                                                  const std::string_view key,
                                                  const ErrorContext context) {
    const std::optional<http::json::Value> value = object.find(key);
    if (!value) {
        return std::optional<http::json::Object>{};
    }
    const Result<http::json::Object> child = value->object();
    if (!child) {
        return fail(ErrorCode::InvalidArgument, context);
    }
    return std::optional<http::json::Object>{*child};
}

}  // namespace config
}  // namespace orion::service
