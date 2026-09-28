#pragma once

// Khung chung của tiến trình server (docs/formats/service.md): dòng lệnh, tệp cấu hình, khoá, log.
// Thứ tự trong main: StopSignal::install (trước mọi luồng), parse_arguments, ConfigFile::load,
// read_common, LogRuntime::start, rồi phần riêng của tiến trình, rồi StopSignal::wait.
//
// Cấu hình và khoá là dữ liệu từ ngoài (X.5): sai thì là Error, không bao giờ là assert. Ngữ cảnh
// của lỗi nêu tên trường, để người vận hành biết sửa gì.

#include "engine/core/error.hpp"
#include "engine/core/log.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/jobs/thread.hpp"
#include "game/server/lib/db/db.hpp"
#include "game/server/lib/http/json.hpp"
#include "game/server/lib/http/server.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace orion::service {

// Cỡ tối đa của tệp cấu hình (cũng là cỡ tối đa của một tài liệu JSON) và của tệp bí mật.
inline constexpr usize kMaxConfigBytes = http::json::kMaxDocumentBytes;
inline constexpr usize kMaxSecretFileBytes = 4'096;

// Môi trường chạy (docs/formats/service.md, mục "Khoá và bí mật"): bí mật dev chỉ nạp được ở
// Local, và không bao giờ trong bản ship.
enum class Environment : u8 {
    Local = 1,
    Production = 2,
};

struct Arguments {
    std::string config_path;
};

// "orion_<tên> --config <tệp>"; `argv` gồm cả tên chương trình ở vị trí 0. Đường dẫn phải là ASCII
// (docs/formats/service.md, mục "Dòng lệnh"). Lỗi: InvalidArgument.
[[nodiscard]] Result<Arguments> parse_arguments(std::span<const char* const> argv);

// Đọc cả tệp, tối đa `max_bytes` byte. Lỗi: như io::File::open, OutOfRange khi tệp lớn hơn.
[[nodiscard]] Result<std::string> read_file(std::string_view path, usize max_bytes);

// Tệp cấu hình JSON đã đọc và parse (docs/formats/http.md, mục "JSON"): gốc là object. Đường dẫn
// tương đối trong cấu hình tính từ thư mục chứa tệp.
class ConfigFile {
public:
    // Lỗi: như read_file (tối đa kMaxConfigBytes), lỗi của http::json::Document::parse,
    // InvalidArgument khi gốc không phải object.
    [[nodiscard]] static Result<ConfigFile> load(std::string_view path);

    [[nodiscard]] http::json::Object root() const noexcept { return root_; }
    // `relative` nếu nó tuyệt đối (bắt đầu bằng '/', '\\' hay "<chữ cái>:", trên mọi nền tảng),
    // không thì nối sau thư mục của tệp cấu hình.
    [[nodiscard]] std::string resolve(std::string_view relative) const;

private:
    ConfigFile(std::string directory, http::json::Document document,
               http::json::Object root) noexcept;

    std::string directory_;
    // root_ trỏ vào cây của document_; cây nằm trên heap nên move ConfigFile không làm cũ nó.
    http::json::Document document_;
    http::json::Object root_;
};

// Đọc từng trường của một object cấu hình. Trường vắng thì lấy `fallback` (hay lỗi, với bản
// required); có mà sai kiểu hay ngoài khoảng thì InvalidArgument với `context`, literal nêu tên
// trường, ví dụ "config: http.port phải là số nguyên 1..65535".
namespace config {
[[nodiscard]] Result<i64> integer(http::json::Object object, std::string_view key, i64 min, i64 max,
                                  i64 fallback, ErrorContext context);
[[nodiscard]] Result<core::Duration> milliseconds(http::json::Object object, std::string_view key,
                                                  i64 min_ms, i64 max_ms, core::Duration fallback,
                                                  ErrorContext context);
[[nodiscard]] Result<i64> required_integer(http::json::Object object, std::string_view key, i64 min,
                                           i64 max, ErrorContext context);
[[nodiscard]] Result<std::string> text(http::json::Object object, std::string_view key,
                                       std::string_view fallback, usize max_bytes,
                                       ErrorContext context);
[[nodiscard]] Result<std::string> required_text(http::json::Object object, std::string_view key,
                                                usize max_bytes, ErrorContext context);
// Object con; nullopt khi vắng.
[[nodiscard]] Result<std::optional<http::json::Object>> section(http::json::Object object,
                                                                std::string_view key,
                                                                ErrorContext context);
}  // namespace config

struct HttpConfig {
    std::string address = "127.0.0.1";
    // `address` của nó bỏ trống; server_config() điền vào.
    http::ServerConfig server;

    // ServerConfig đầy đủ; address trỏ vào HttpConfig này, nên dùng khi nó còn sống.
    [[nodiscard]] http::ServerConfig server_config() const noexcept;
};

struct DatabaseConfig {
    // Chuỗi kết nối libpq, đọc từ tệp bí mật; bị ghi 0 khi huỷ hay khi bị move đi.
    std::string conninfo;
    db::Options options;
    core::Duration connect_timeout = core::Duration::seconds(5);

    DatabaseConfig() = default;
    DatabaseConfig(const DatabaseConfig&) = delete;
    DatabaseConfig& operator=(const DatabaseConfig&) = delete;
    DatabaseConfig(DatabaseConfig&& other) noexcept;
    DatabaseConfig& operator=(DatabaseConfig&& other) noexcept;
    ~DatabaseConfig();
};

// Phần chung của mọi tệp cấu hình (docs/formats/service.md, mục "Cấu hình").
struct CommonConfig {
    Environment environment = Environment::Local;
    core::LogLevel log_level = core::LogLevel::Info;
    // Có khi tệp có mục "http".
    std::optional<HttpConfig> http;
    // Có khi tệp có mục "database".
    std::optional<DatabaseConfig> database;
};

// Đọc "environment", "log", "http", "database" ở gốc. Trường lạ ở gốc thì lỗi, trừ các tên trong
// `service_sections` (mục riêng của tiến trình).
[[nodiscard]] Result<CommonConfig> read_common(const ConfigFile& file,
                                               std::span<const std::string_view> service_sections);

// Tệp khoá (docs/formats/service.md, mục "Khoá và bí mật"): 64 chữ số hex và một dòng mới tuỳ
// chọn. Tệp có ".dev." trong tên là khoá dev: chỉ nạp ở Environment::Local, và bản ship
// (ORION_SHIP) từ chối luôn. crypto::initialize() phải đã thành công. Lỗi: như read_file,
// InvalidArgument khi sai định dạng, FailedPrecondition với khoá dev ngoài Local hay trong bản
// ship.
[[nodiscard]] Result<crypto::SigningKeyPair> load_signing_key(std::string_view path,
                                                              Environment environment);
[[nodiscard]] Result<crypto::SigningPublicKey> load_public_key(std::string_view path,
                                                               Environment environment);

// Logger toàn cục của tiến trình (CLAUDE.md X.3, ngoại lệ duy nhất cùng allocator gốc), với một
// luồng ghi riêng (ADR 0007); trong main, sink là core::JsonLinesSink ra stdout (X.5). Dựng một lần
// trong main, ngay sau StopSignal::install, trước mọi luồng khác (engine/core/log.hpp). Huỷ sau khi
// mọi luồng có thể log đã dừng: huỷ thì gỡ logger toàn cục, ghi nốt log còn chờ, rồi chờ luồng ghi.
class LogRuntime {
    // Chỉ start dựng được: logger phải có địa chỉ cố định trước khi luồng ghi chạy.
    struct Token {
        explicit Token() = default;
    };

public:
    // `clock` và `sink` phải sống lâu hơn LogRuntime; chỉ luồng ghi gọi `sink`. Lỗi:
    // ResourceExhausted khi không tạo được luồng ghi.
    [[nodiscard]] static Result<std::unique_ptr<LogRuntime>> start(core::LogLevel level,
                                                                   const core::WallClock& clock,
                                                                   core::LogSink& sink);

    LogRuntime(Token token, core::LogLevel level, const core::WallClock& clock);
    LogRuntime(const LogRuntime&) = delete;
    LogRuntime& operator=(const LogRuntime&) = delete;
    LogRuntime(LogRuntime&&) = delete;
    LogRuntime& operator=(LogRuntime&&) = delete;
    ~LogRuntime();

private:
    core::Logger logger_;
    jobs::Thread thread_;
};

}  // namespace orion::service
