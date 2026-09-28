#include "game/server/lib/http/detail/listener.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>

namespace orion::http::detail {
namespace {

// Tuỳ chọn socket SO_EXCLUSIVEADDRUSE theo yêu cầu SettableSocketOption của Asio; winsock2.h (qua
// Asio) khai hai hằng và kiểu BOOL (int) của giá trị.
class ExclusiveAddressUse {
public:
    template <class Protocol>
    [[nodiscard]] int level(const Protocol& /*protocol*/) const noexcept {
        return SOL_SOCKET;
    }
    template <class Protocol>
    [[nodiscard]] int name(const Protocol& /*protocol*/) const noexcept {
        return SO_EXCLUSIVEADDRUSE;
    }
    template <class Protocol>
    [[nodiscard]] const void* data(const Protocol& /*protocol*/) const noexcept {
        return &value_;
    }
    template <class Protocol>
    [[nodiscard]] std::size_t size(const Protocol& /*protocol*/) const noexcept {
        return sizeof(value_);
    }

private:
    int value_ = 1;
};

}  // namespace

void configure_listener(boost::asio::ip::tcp::acceptor& acceptor,
                        boost::system::error_code& error) noexcept {
    static_cast<void>(acceptor.set_option(ExclusiveAddressUse{}, error));
}

}  // namespace orion::http::detail
