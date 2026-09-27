// Fuzz code do tools/codegen sinh ra (CLAUDE.md X.4, X.9, X.16.9), trên protocol thử
// game/shared/tests/protocol/everything.schema: protocol dùng mọi kiểu và mọi thuộc tính của ngôn
// ngữ schema, nên mọi dạng code đọc mà codegen sinh ra đều có mặt. Cách đọc input và bất biến ở
// tests/fuzz/support/protocol_fuzz.hpp.

#include "game/shared/tests/generated/test_protocol.hpp"
#include "tests/fuzz/support/protocol_fuzz.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace protocol = orion::protocol::testing;
    orion::fuzz::decode_one<std::max(protocol::kMaxMessageSize, protocol::kMaxEventSize)>(
        data, size, protocol::decode_client_message, protocol::decode_server_message,
        protocol::decode_event);
    return 0;
}
