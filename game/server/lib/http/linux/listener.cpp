#include "game/server/lib/http/detail/listener.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/system/error_code.hpp>

namespace orion::http::detail {

void configure_listener(boost::asio::ip::tcp::acceptor& acceptor,
                        boost::system::error_code& error) noexcept {
    static_cast<void>(acceptor.set_option(boost::asio::socket_base::reuse_address(true), error));
}

}  // namespace orion::http::detail
