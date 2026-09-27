// Fuzz bộ đọc của protocol game/shared/protocol/*.schema (ADR 0004 mục 6; CLAUDE.md X.4, X.9,
// X.16.9): ba điểm vào giải mã tin nhắn từ mạng và sự kiện từ Redis Streams. Cách đọc input và bất
// biến ở tests/fuzz/support/protocol_fuzz.hpp.

#include "game/shared/protocol/protocol.hpp"
#include "tests/fuzz/support/protocol_fuzz.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace protocol = orion::protocol;
    orion::fuzz::decode_one<std::max(protocol::kMaxMessageSize, protocol::kMaxEventSize)>(
        data, size, protocol::decode_client_message, protocol::decode_server_message,
        protocol::decode_event);
    return 0;
}
