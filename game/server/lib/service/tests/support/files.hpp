#pragma once

// Tệp cho test của server/lib/service. Tên tệp tạm lấy từ io::testing::TempPath: gồm tên test đang
// chạy, nên các tiến trình test chạy song song dưới ctest không đụng nhau.

#include "engine/core/error.hpp"
#include "engine/io/file.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>

namespace orion::service::testing {

// Ghi `text` thành toàn bộ nội dung của `path`.
inline void write_file(const std::string& path, const std::string_view text) {
    Result<io::AtomicFileWriter> writer = io::AtomicFileWriter::create(path);
    ASSERT_TRUE(writer.has_value()) << path;
    ASSERT_TRUE(writer->write(std::as_bytes(std::span(text))).has_value()) << path;
    ASSERT_TRUE(writer->commit().has_value()) << path;
}

// Phần sau dấu '/' hay '\' cuối của `path`.
[[nodiscard]] inline std::string_view file_name(const std::string_view path) {
    const std::string_view::size_type slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace orion::service::testing
