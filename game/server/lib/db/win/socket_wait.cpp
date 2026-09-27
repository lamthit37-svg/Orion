// Chờ socket của libpq trên Windows: WSAPoll. PQsocket trả SOCKET của libpq đổi sang int (tài liệu
// libpq), nên đổi ngược là đúng giá trị.

#include "game/server/lib/db/detail/socket_wait.hpp"

#include "engine/core/error.hpp"

#include <winsock2.h>

#include <cstdint>

namespace orion::db::detail {

Result<bool> wait_socket(const std::intptr_t socket, const Wait wait,
                         const int timeout_ms) noexcept {
    WSAPOLLFD entry{};
    entry.fd = static_cast<SOCKET>(socket);
    entry.events = static_cast<SHORT>((wait != Wait::Write ? POLLRDNORM : 0) |
                                      (wait != Wait::Read ? POLLWRNORM : 0));
    const int ready = WSAPoll(&entry, 1, timeout_ms);
    if (ready == SOCKET_ERROR) {
        return fail(ErrorCode::Io, "db: WSAPoll thất bại", WSAGetLastError());
    }
    return ready > 0;
}

}  // namespace orion::db::detail
