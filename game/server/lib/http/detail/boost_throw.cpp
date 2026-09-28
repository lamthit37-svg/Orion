// Boost build không exception (BOOST_NO_EXCEPTIONS, tự bật khi thiếu -fexceptions hay /EHsc) gọi
// boost::throw_exception ở chỗ lẽ ra ném, và bên dùng phải định nghĩa hàm này (ADR 0015, quyết định
// 3). server_http chỉ gọi các overload nhận error_code và tự kiểm mọi điều kiện mà Beast ném (giới
// hạn của Limits, header của Response, body của 204), nên tới được đây là lỗi lập trình hay hết tài
// nguyên lúc khởi động: crash kèm minidump (CLAUDE.md X.5).

#include "engine/core/assert.hpp"

#include <boost/assert/source_location.hpp>
#include <boost/throw_exception.hpp>

#include <cstdlib>
#include <exception>

namespace boost {

void throw_exception(const std::exception& error) {
    ORION_VERIFY(false, "Boost báo lỗi không phục hồi được: {}", error.what());
    std::abort();
}

void throw_exception(const std::exception& error, const boost::source_location& location) {
    ORION_VERIFY(false, "Boost báo lỗi không phục hồi được: {} ({}:{})", error.what(),
                 location.file_name(), location.line());
    std::abort();
}

}  // namespace boost
