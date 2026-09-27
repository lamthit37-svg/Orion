// Phần chung của các fuzz target cho bộ đọc do tools/codegen sinh ra (CLAUDE.md X.4, X.9, X.16.9;
// docs/formats/protocol.md, mục Tin nhắn). Byte đầu của input chọn bên đọc (message của client, của
// server, hay event) và kênh; phần còn lại là tin nhắn từ mạng.
//
// Bất biến: bên đọc là hàm toàn phần, mọi input cho ra một tin nhắn hay một lỗi trong bảng
// (OutOfRange, InvalidArgument). Tin nhắn được nhận thì mã hoá lại phải ra đúng từng byte của
// input: mỗi tin nhắn chỉ có một cách mã hoá. ASan và UBSan của preset fuzz bắt phần còn lại.

#pragma once

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/channels.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <variant>
#include <vector>

namespace orion::fuzz {

inline constexpr std::array kChannels = {net::Channel::Unreliable, net::Channel::Sequenced,
                                         net::Channel::ReliableOrdered,
                                         net::Channel::ReliableUnordered};

// `encode` tìm theo ADL trong namespace của tin nhắn; BufferSize đủ cho tin nhắn lớn nhất.
template <usize BufferSize, class Group>
void check_decoded(const Result<Group>& decoded, const std::span<const std::byte> input) {
    if (!decoded.has_value()) {
        const ErrorCode code = decoded.error().code();
        ORION_VERIFY(code == ErrorCode::OutOfRange || code == ErrorCode::InvalidArgument,
                     "mã lỗi ngoài bảng của định dạng");
        return;
    }
    std::array<std::byte, BufferSize> buffer{};
    const Result<usize> size =
        std::visit([&buffer](const auto& message) { return encode(message, buffer); }, *decoded);
    ORION_VERIFY(size.has_value(), "tin nhắn đã nhận phải mã hoá lại được");
    ORION_VERIFY(std::ranges::equal(std::span(buffer).first(*size), input),
                 "mã hoá lại không ra đúng input: có hai cách mã hoá");
}

// Một input: client(kênh, byte), server(kênh, byte) và event(byte) là ba hàm giải mã sinh ra.
template <usize BufferSize, class Client, class Server, class Event>
void decode_one(const std::uint8_t* data, const std::size_t size, const Client& client,
                const Server& server, const Event& event) {
    if (size == 0) {
        return;
    }
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size - 1);
    if (size > 1) {
        std::memcpy(input.data(), data + 1, size - 1);
    }
    const net::Channel channel = kChannels[data[0] % kChannels.size()];
    switch ((data[0] / kChannels.size()) % 3) {
        case 0:
            check_decoded<BufferSize>(client(channel, input), input);
            break;
        case 1:
            check_decoded<BufferSize>(server(channel, input), input);
            break;
        default:
            check_decoded<BufferSize>(event(input), input);
            break;
    }
}

}  // namespace orion::fuzz
