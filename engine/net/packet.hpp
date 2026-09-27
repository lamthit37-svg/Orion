#pragma once

// Gói transport trên dây (docs/formats/transport.md): loại gói ở byte đầu, và gói dữ liệu mã hoá
// AEAD với nonce lấy từ số thứ tự, header làm associated data (CLAUDE.md X.9). Không giữ trạng
// thái: số thứ tự, khoá và cửa sổ chống replay thuộc kết nối.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/aead.hpp"

#include <cstddef>
#include <span>

namespace orion::net {

// Cỡ tối đa của một gói UDP của transport: vừa MTU tối thiểu của IPv6 (mục Gói của định dạng).
inline constexpr usize kMaxPacketSize = 1200;
// Header của gói dữ liệu: prefix, connection id, tới 8 byte số thứ tự.
inline constexpr usize kMaxDataHeaderSize = 1 + 4 + 8;
// Payload lớn nhất mà mọi số thứ tự đều chở được.
inline constexpr usize kMaxDataPayload = kMaxPacketSize - kMaxDataHeaderSize - crypto::kAeadTagSize;

enum class PacketType : u8 {
    Request = 1,
    Challenge = 2,
    Response = 3,
    Accept = 4,
    Reject = 5,
    Data = 6,
};

// Loại của một gói nhận được, đọc từ 4 bit thấp của byte đầu; không kiểm gì khác. Lỗi
// InvalidArgument khi gói rỗng, dài hơn kMaxPacketSize hay loại lạ.
[[nodiscard]] Result<PacketType> packet_type(std::span<const std::byte> packet) noexcept;

// Header của một gói dữ liệu, đọc được trước khi xác thực để tra kết nối. Chưa có gì trong đó đáng
// tin cho tới khi open_data_packet qua.
struct DataHeader {
    u32 connection_id = 0;
    u64 sequence = 0;
    // Số byte của header trên dây.
    usize size = 0;
};

// Đọc và kiểm header của gói dữ liệu (bước 1 của mục Nhận). Hàm toàn phần; lỗi InvalidArgument.
[[nodiscard]] Result<DataHeader> read_data_header(std::span<const std::byte> packet) noexcept;

// Ghi gói dữ liệu vào đầu `out` và trả cỡ của nó. Lỗi InvalidArgument khi connection_id là 0,
// payload vượt chỗ còn lại của kMaxPacketSize sau header và tag, hay `out` thiếu chỗ.
[[nodiscard]] Result<usize> seal_data_packet(std::span<std::byte> out, u32 connection_id,
                                             u64 sequence, std::span<const std::byte> payload,
                                             const crypto::AeadKey& key) noexcept;

// Kiểm header, xác thực và giải mã `packet` bằng `key`; ghi payload vào đầu `out` và trả số byte
// của nó. Dữ liệu từ mạng đi thẳng vào được. Lỗi: InvalidArgument (header sai, `out` thiếu chỗ),
// DataLoss (tag sai; khi đó `out` không chứa gì chưa xác thực). Không hỏi cửa sổ chống replay:
// bên gọi làm việc đó theo mục Nhận của định dạng.
[[nodiscard]] Result<usize> open_data_packet(std::span<std::byte> out,
                                             std::span<const std::byte> packet,
                                             const crypto::AeadKey& key) noexcept;

}  // namespace orion::net
