// Fuzz code do tools/codegen sinh ra (CLAUDE.md X.4, X.9, X.16.9; docs/formats/protocol.md, mục Tin
// nhắn), trên protocol thử game/shared/tests/protocol/everything.schema: protocol dùng mọi kiểu và
// mọi thuộc tính của ngôn ngữ schema, nên mọi dạng code đọc mà codegen sinh ra đều có mặt. Byte đầu
// của input chọn bên đọc (message của client, của server, hay event) và kênh; phần còn lại là tin
// nhắn từ mạng.
//
// Bất biến: bên đọc là hàm toàn phần, mọi input cho ra một tin nhắn hay một lỗi trong bảng
// (OutOfRange, InvalidArgument). Tin nhắn được nhận thì mã hoá lại phải ra đúng từng byte của
// input: mỗi tin nhắn chỉ có một cách mã hoá. ASan và UBSan của preset fuzz bắt phần còn lại.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/channels.hpp"
#include "game/shared/tests/generated/test_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <variant>
#include <vector>

namespace {

using orion::ErrorCode;
using orion::Result;
using orion::usize;
namespace net = orion::net;
namespace protocol = orion::protocol::testing;

constexpr std::array kChannels = {net::Channel::Unreliable, net::Channel::Sequenced,
                                  net::Channel::ReliableOrdered, net::Channel::ReliableUnordered};
constexpr usize kMaxSize = std::max(protocol::kMaxMessageSize, protocol::kMaxEventSize);

template <class Group>
void check(const Result<Group>& decoded, const std::span<const std::byte> input) {
    if (!decoded.has_value()) {
        const ErrorCode code = decoded.error().code();
        ORION_VERIFY(code == ErrorCode::OutOfRange || code == ErrorCode::InvalidArgument,
                     "mã lỗi ngoài bảng của định dạng");
        return;
    }
    std::array<std::byte, kMaxSize> buffer{};
    const Result<usize> size =
        std::visit([&buffer](const auto& message) { return encode(message, buffer); }, *decoded);
    ORION_VERIFY(size.has_value(), "tin nhắn đã nhận phải mã hoá lại được");
    ORION_VERIFY(std::ranges::equal(std::span(buffer).first(*size), input),
                 "mã hoá lại không ra đúng input: có hai cách mã hoá");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size - 1);
    if (size > 1) {
        std::memcpy(input.data(), data + 1, size - 1);
    }
    const net::Channel channel = kChannels[data[0] % kChannels.size()];
    switch ((data[0] / kChannels.size()) % 3) {
        case 0:
            check(protocol::decode_client_message(channel, input), input);
            break;
        case 1:
            check(protocol::decode_server_message(channel, input), input);
            break;
        default:
            check(protocol::decode_event(input), input);
            break;
    }
    return 0;
}
