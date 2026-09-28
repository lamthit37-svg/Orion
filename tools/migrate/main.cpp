// orion_migrate: áp db/migrations/ vào PostgreSQL (docs/formats/migrations.md, CLAUDE.md X.14).
//
//   orion_migrate [--dir <thư mục>] [--check] [--timeout <giây>]
//
// Chuỗi kết nối ở biến môi trường ORION_DATABASE, không ở dòng lệnh: ai trên máy cũng đọc được dòng
// lệnh của tiến trình, mà chuỗi kết nối có thể có mật khẩu. Mã thoát: 0 khi DB khớp code, 1 khi
// --check thấy còn migration chưa áp, 2 khi lỗi.

#include "engine/core/environment.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "game/server/lib/db/db.hpp"
#include "tools/migrate/migrate.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using orion::i64;
using orion::core::Duration;

constexpr int kSynced = 0;
constexpr int kPending = 1;
constexpr int kFailed = 2;
constexpr Duration kConnectTimeout = Duration::seconds(30);
constexpr i64 kMaxTimeoutSeconds = 86'400;

constexpr std::string_view kUsage =
    "cách dùng: orion_migrate [--dir <thư mục>] [--check] [--timeout <giây>]\n"
    "chuỗi kết nối PostgreSQL ở biến môi trường ORION_DATABASE\n";

struct Arguments {
    std::filesystem::path dir = "db/migrations";
    bool check = false;
    Duration timeout = orion::migrate::Options{}.timeout;
};

// Ghi rồi flush ngay, để khi stdout và stderr cùng vào một ống, thứ tự dòng vẫn đúng thứ tự sự
// việc: các migration đã áp trước, lỗi sau. false khi ghi không trọn (ống đã đóng, đĩa đầy).
bool print(std::FILE* stream, const std::string_view text) {
    const bool written = std::fwrite(text.data(), 1, text.size(), stream) == text.size();
    return std::fflush(stream) == 0 && written;
}

[[nodiscard]] std::optional<i64> seconds_of(const std::string_view text) {
    i64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 1 ||
        value > kMaxTimeoutSeconds) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<Arguments> parse_arguments(const std::span<char*> argv) {
    Arguments arguments;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string_view option(argv[i]);
        const bool has_value = i + 1 < argv.size();
        if (option == "--check") {
            arguments.check = true;
        } else if (option == "--dir" && has_value) {
            // Dựng path từ chuỗi hẹp: trên Windows argv theo code page ANSI, đúng thứ path hiểu.
            arguments.dir = std::filesystem::path(argv[++i]);
        } else if (option == "--timeout" && has_value) {
            const std::optional<i64> seconds = seconds_of(argv[++i]);
            if (!seconds) {
                return std::nullopt;
            }
            arguments.timeout = Duration::seconds(*seconds);
        } else {
            return std::nullopt;
        }
    }
    return arguments;
}

// Lỗi từ DB mang thông điệp của server, và detail là SQLSTATE gói thành số: in lại dạng chữ.
int report(const orion::migrate::Failure& failure) {
    const orion::Error& error = failure.error;
    std::string line =
        std::format("orion_migrate: {}: {} [{}]", error.code(), error.context(), failure.subject);
    if (!failure.message.empty()) {
        line += std::format(": {}", failure.message);
        if (error.detail() != 0) {
            const std::array<char, 5> state = orion::db::sqlstate_text(error.detail());
            line += std::format(" (SQLSTATE {})", std::string_view(state.data(), state.size()));
        }
    } else if (error.detail() != 0) {
        line += std::format(" (chi tiết {})", error.detail());
    }
    line += '\n';
    print(stderr, line);
    return kFailed;
}

int run(const Arguments& arguments) {
    if (!orion::crypto::initialize()) {
        print(stderr, "orion_migrate: không khởi tạo được libsodium\n");
        return kFailed;
    }
    const std::optional<std::string> conninfo = orion::core::environment_variable("ORION_DATABASE");
    if (!conninfo) {
        print(stderr, "orion_migrate: chưa đặt biến môi trường ORION_DATABASE\n");
        return kFailed;
    }
    const orion::migrate::Outcome<std::vector<orion::migrate::Migration>> migrations =
        orion::migrate::load(arguments.dir);
    if (!migrations) {
        return report(migrations.error());
    }
    const orion::core::SystemMonotonicClock clock;
    // Không đặt statement_timeout: mỗi migration đã có hạn riêng (--timeout).
    orion::db::Connection connection(
        clock, orion::db::Options{.statement_timeout = Duration{},
                                  .client_check_interval = Duration::seconds(1),
                                  .application_name = "orion_migrate"});
    if (const orion::Result<void> connected =
            connection.connect(*conninfo, clock.now() + kConnectTimeout);
        !connected) {
        return report({connected.error(), "ORION_DATABASE", std::string(connection.last_error())});
    }
    const orion::migrate::Options options{.timeout = arguments.timeout};
    if (arguments.check) {
        const orion::migrate::Outcome<orion::migrate::Status> status =
            orion::migrate::check(connection, *migrations, clock, options);
        if (!status) {
            return report(status.error());
        }
        if (!print(stdout, std::format("orion_migrate: đã áp {}, còn {} chưa áp\n", status->applied,
                                       status->pending))) {
            return kFailed;
        }
        return status->pending == 0 ? kSynced : kPending;
    }
    // Migration đã commit thì vẫn được tính dù in ra lỗi; chỉ mã thoát báo việc in hỏng.
    bool printed = true;
    const orion::migrate::Outcome<orion::migrate::Status> status = orion::migrate::apply(
        connection, *migrations, clock, options,
        [&printed](const orion::migrate::Migration& migration) {
            printed =
                print(stdout, std::format("orion_migrate: đã áp {}\n", migration.file_name())) &&
                printed;
        });
    if (!status) {
        return report(status.error());
    }
    printed = print(stdout, std::format("orion_migrate: DB ở phiên bản {}\n", status->applied)) &&
              printed;
    return printed ? kSynced : kFailed;
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Arguments> arguments =
        parse_arguments(std::span(argv, static_cast<std::size_t>(argc)));
    if (!arguments) {
        print(stderr, kUsage);
        return kFailed;
    }
    return run(*arguments);
}
