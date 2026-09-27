// Chờ socket của libpq trên Linux: poll.

#include "game/server/lib/db/detail/socket_wait.hpp"

#include "engine/core/error.hpp"

#include <sys/poll.h>

#include <cerrno>
#include <cstdint>

namespace orion::db::detail {

Result<bool> wait_socket(const std::intptr_t socket, const Wait wait,
                         const int timeout_ms) noexcept {
    pollfd entry{};
    entry.fd = static_cast<int>(socket);
    entry.events =
        static_cast<short>((wait != Wait::Write ? POLLIN : 0) | (wait != Wait::Read ? POLLOUT : 0));
    const int ready = ::poll(&entry, 1, timeout_ms);
    if (ready < 0) {
        // Bị tín hiệu ngắt: trả false như hết hạn, để bên gọi xem lại mốc hạn rồi chờ tiếp.
        if (errno == EINTR) {
            return false;
        }
        return fail(ErrorCode::Io, "db: poll thất bại", errno);
    }
    return ready > 0;
}

}  // namespace orion::db::detail
