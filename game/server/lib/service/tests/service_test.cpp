// Dòng lệnh, đọc tệp, tệp cấu hình (docs/formats/service.md, mục "Dòng lệnh", "Cấu hình").

#include "game/server/lib/service/service.hpp"

#include "engine/core/error.hpp"
#include "engine/io/tests/support/temp_file.hpp"
#include "game/server/lib/http/json.hpp"
#include "game/server/lib/service/tests/support/files.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace orion::service {
namespace {

using io::testing::TempPath;

// Mã lỗi của parse_arguments(argv); nullopt khi thành công.
[[nodiscard]] std::optional<ErrorCode> parse_error(const std::initializer_list<const char*> argv) {
    const Result<Arguments> arguments = parse_arguments(std::span(argv.begin(), argv.end()));
    if (arguments) {
        return std::nullopt;
    }
    return arguments.error().code();
}

TEST(ParseArguments, ReadsTheConfigPath) {
    const std::initializer_list<const char*> argv{"orion_auth", "--config",
                                                  "deploy/local/auth.json"};
    const Result<Arguments> arguments = parse_arguments(std::span(argv.begin(), argv.end()));
    ASSERT_TRUE(arguments.has_value());
    EXPECT_EQ(arguments->config_path, "deploy/local/auth.json");
}

TEST(ParseArguments, RejectsAnythingButExactlyOneConfigOption) {
    EXPECT_EQ(parse_error({}), ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth"}), ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "--config"}), ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "--config", ""}), ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "auth.json"}), ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "--port", "80"}), ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "--config", "a.json", "--config", "b.json"}),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "--config", "a.json", "extra"}),
              ErrorCode::InvalidArgument);
}

TEST(ParseArguments, RejectsNonAsciiPaths) {
    EXPECT_EQ(parse_error({"orion_auth", "--config", "c\xC3\xA1u-h\xC3\xACnh.json"}),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_error({"orion_auth", "--config", "\x80.json"}), ErrorCode::InvalidArgument);
}

TEST(ReadFile, ReadsTheWholeFileUpToTheLimit) {
    const TempPath path("data.txt");
    testing::write_file(path.path(), "0123456789");
    const Result<std::string> exact = read_file(path.path(), 10);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(*exact, "0123456789");

    const Result<std::string> over = read_file(path.path(), 9);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(over.error().detail(), 10);
}

TEST(ReadFile, ReadsAnEmptyFile) {
    const TempPath path("empty.txt");
    testing::write_file(path.path(), "");
    const Result<std::string> text = read_file(path.path(), 10);
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(text->empty());
}

TEST(ReadFile, MissingFileIsNotFound) {
    const TempPath path("missing.txt");
    const Result<std::string> text = read_file(path.path(), 10);
    ASSERT_FALSE(text.has_value());
    EXPECT_EQ(text.error().code(), ErrorCode::NotFound);
}

TEST(ConfigFile, LoadsAnObjectRootThatSurvivesMoves) {
    const TempPath path("config.json");
    testing::write_file(path.path(), R"({"environment":"local","auth":{"token_ttl_s":30}})");
    Result<ConfigFile> file = ConfigFile::load(path.path());
    ASSERT_TRUE(file.has_value());
    const http::json::Object root = file->root();
    EXPECT_EQ(root.size(), 2U);

    // Object lấy trước khi move vẫn đọc được sau đó: cây của tài liệu nằm trên heap.
    const ConfigFile moved = std::move(*file);
    const Result<http::json::Value> environment = root.at("environment");
    ASSERT_TRUE(environment.has_value());
    const Result<std::string_view> name = environment->string();
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "local");
    EXPECT_EQ(moved.root().size(), 2U);
}

TEST(ConfigFile, RejectsTextThatIsNotAJsonObject) {
    const TempPath path("config.json");
    for (const std::string_view text :
         {"", "[]", "\"local\"", "{", R"({"a":1,"a":2})", R"({"a":1} {})"}) {
        testing::write_file(path.path(), text);
        const Result<ConfigFile> file = ConfigFile::load(path.path());
        ASSERT_FALSE(file.has_value()) << text;
        EXPECT_EQ(file.error().code(), ErrorCode::InvalidArgument) << text;
    }
}

TEST(ConfigFile, RejectsFilesOverTheLimit) {
    const TempPath path("config.json");
    std::string text(kMaxConfigBytes + 1, ' ');
    text.front() = '{';
    text.back() = '}';
    testing::write_file(path.path(), text);
    const Result<ConfigFile> file = ConfigFile::load(path.path());
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().code(), ErrorCode::OutOfRange);
}

TEST(ConfigFile, MissingFileIsNotFound) {
    const TempPath path("config.json");
    const Result<ConfigFile> file = ConfigFile::load(path.path());
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().code(), ErrorCode::NotFound);
}

TEST(ConfigFile, ResolvesRelativePathsFromItsDirectory) {
    const TempPath path("config.json");
    testing::write_file(path.path(), "{}");
    const Result<ConfigFile> file = ConfigFile::load(path.path());
    ASSERT_TRUE(file.has_value());
    const std::string_view full = path.path();
    const std::string directory(full.substr(0, full.size() - testing::file_name(full).size() - 1));
    EXPECT_EQ(file->resolve("keys/auth.key"), directory + "/keys/auth.key");
    EXPECT_EQ(file->resolve("auth.key"), directory + "/auth.key");
    // Tuyệt đối theo cùng một luật trên mọi nền tảng.
    for (const std::string_view absolute : {"/etc/orion/auth.key", R"(\\server\keys\auth.key)",
                                            "C:/keys/auth.key", "d:\\keys\\auth.key"}) {
        EXPECT_EQ(file->resolve(absolute), absolute);
    }
    // Chữ số trước ':' không phải ổ đĩa.
    EXPECT_EQ(file->resolve("1:x"), directory + "/1:x");
}

TEST(ConfigFile, InTheWorkingDirectoryKeepsRelativePathsAsTheyAre) {
    const TempPath path("", "config.json");
    testing::write_file(path.path(), "{}");
    const Result<ConfigFile> file = ConfigFile::load(path.path());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->resolve("keys/auth.key"), "keys/auth.key");
}

}  // namespace
}  // namespace orion::service
