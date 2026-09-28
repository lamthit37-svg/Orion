#pragma once

// Tuỳ chọn socket nghe theo nền tảng (nội bộ của server_http; cài trong linux/ và win/).

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

namespace orion::http::detail {

// Gọi sau open, trước bind. Linux: SO_REUSEADDR, để server khởi động lại bind được ngay dù kết nối
// cũ còn ở TIME_WAIT. Windows: SO_EXCLUSIVEADDRUSE, để tiến trình khác không bind chồng lên cổng
// (SO_REUSEADDR của Windows cho phép điều đó).
void configure_listener(boost::asio::ip::tcp::acceptor& acceptor,
                        boost::system::error_code& error) noexcept;

}  // namespace orion::http::detail
