#pragma once

// Tệp tạm cho test của engine/io: tên gồm tên test đang chạy, nên các tiến trình test chạy song
// song dưới ctest không đụng nhau; tự xoá khi ra khỏi phạm vi.

#include "engine/io/detail/native_file.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace orion::io::testing {

class TempPath {
public:
    explicit TempPath(const std::string_view suffix) {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = ::testing::TempDir() + "orion_io_" + info->test_suite_name() + "_" + info->name() +
                "_" + std::string(suffix);
        static_cast<void>(detail::remove_file(path_.c_str()).has_value());
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    TempPath(TempPath&&) = delete;
    TempPath& operator=(TempPath&&) = delete;
    ~TempPath() { static_cast<void>(detail::remove_file(path_.c_str()).has_value()); }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

}  // namespace orion::io::testing
