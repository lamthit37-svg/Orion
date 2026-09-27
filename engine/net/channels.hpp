#pragma once

// Lớp kênh tin nhắn của một kết nối (docs/formats/channels.md, ADR 0004 mục 3): unreliable,
// sequenced, reliable_ordered, reliable_unordered. Gom tin nhắn thành gói (dữ liệu tầng trên của
// transport), xác nhận gói, gửi lại mảnh tin cậy chưa được xác nhận, cắt và ghép tin nhắn lớn.
//
// - Không chạm transport hay socket, nhận thời gian từ bên gọi (X.4): bên gọi đưa payload nhận
//   được vào read_packet, và gửi qua transport những gì write_packet ghi ra.
// - Mọi bộ đệm cấp một lần lúc create; send, write_packet, read_packet không cấp phát (X.7). Bộ
//   nhớ tin nhắn tin cậy là ngân sách byte khai trong ChannelsConfig, không tỉ lệ với cỡ tin nhắn
//   lớn nhất: bên gửi đặt mỗi tin nhắn vào một đoạn của bộ đệm nhận của bên kia (mục Bộ đệm của
//   định dạng).
// - read_packet kiểm toàn bộ gói rồi mới áp (X.10): gói sai luật bị bỏ nguyên vẹn, không đổi gì.
// - Không đồng bộ: thuộc luồng xử lý kết nối đó.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/connection.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace orion::net {

enum class Channel : u8 {
    Unreliable = 0,
    Sequenced = 1,
    ReliableOrdered = 2,
    ReliableUnordered = 3,
};

inline constexpr usize kMaxUnreliableMessageSize = 1150;
inline constexpr usize kFragmentSize = 1024;
inline constexpr u32 kMaxFragments = 32;
inline constexpr usize kMaxReliableMessageSize = kFragmentSize * kMaxFragments;
// Bộ đệm của một kênh tin cậy, mỗi chiều; offset trên dây có 20 bit.
inline constexpr usize kMaxReliableBufferSize = usize{1} << 20U;
inline constexpr u32 kMaxMessagesPerPacket = 64;
// Chính sách, không phải số đo (mục Xác nhận và gửi lại của định dạng).
inline constexpr u32 kReliableWindow = 64;
inline constexpr u32 kSentPacketHistory = 64;
inline constexpr core::Duration kInitialRtt = core::Duration::milliseconds(100);
inline constexpr core::Duration kMinResendDelay = core::Duration::milliseconds(50);
inline constexpr core::Duration kAckDelay = core::Duration::milliseconds(25);
// Byte tin nhắn unreliable và sequenced chờ gói kế tiếp.
inline constexpr usize kUnreliableQueueBytes = usize{8} * 1024;

// Hai đầu của một kết nối khai cấu hình đối xứng: send_buffer_size của đầu này là
// receive_buffer_size của đầu kia. Cả hai là hằng của protocol, không thương lượng trên dây.
struct ChannelsConfig {
    // Byte mỗi kênh tin cậy đầu này giữ cho tin nhắn nhận chưa giao, 1 tới kMaxReliableBufferSize.
    // Tin nhắn tin cậy đầu này nhận không lớn hơn min(số này, kMaxReliableMessageSize).
    usize receive_buffer_size = 0;
    // receive_buffer_size của đầu kia: byte tin nhắn tin cậy đang bay của mỗi kênh không vượt số
    // này.
    usize send_buffer_size = 0;
};

struct ReceivedMessage {
    Channel channel = Channel::Unreliable;
    // Trỏ vào bộ đệm của MessageChannels; chỉ sống tới lần gọi read_packet kế tiếp.
    std::span<const std::byte> data;
};

class MessageChannels {
public:
    // Cấp mọi bộ đệm: 2 × (receive_buffer_size + send_buffer_size) byte cộng phần cố định khoảng
    // 76 KiB trên Linux x64 (sổ 64 gói đã gửi, thời điểm gửi từng mảnh, hàng unreliable, bộ đệm
    // đọc gói). Lỗi InvalidArgument khi một cỡ ngoài [1, kMaxReliableBufferSize].
    [[nodiscard]] static Result<MessageChannels> create(const ChannelsConfig& config);

    // Xếp một tin nhắn để gửi. Unreliable và sequenced đi ở gói kế tiếp còn chỗ; tin cậy được gửi
    // lại tới khi mọi mảnh được xác nhận. Lỗi: InvalidArgument (rỗng hay quá cỡ của kênh; tin cậy
    // tới min(send_buffer_size, kMaxReliableMessageSize)), ResourceExhausted (kênh tin cậy có
    // kReliableWindow tin nhắn chưa xác nhận hết hay bộ đệm gửi hết chỗ liền, hay hàng unreliable
    // đầy): tầng trên giảm nhịp và gửi lại sau.
    [[nodiscard]] Result<void> send(Channel channel, std::span<const std::byte> message) noexcept;

    // Ghi gói kế tiếp vào `out` và trả cỡ của nó; 0 khi không có gì cần gửi. Gọi lặp tới khi trả 0
    // (hay tới ngân sách gói của nhịp đó).
    [[nodiscard]] usize write_packet(std::span<std::byte, kMaxTransportPayload> out,
                                     core::MonoTime now) noexcept;

    // Đọc một gói nhận được; trả các tin nhắn giao được ngay. Lỗi theo bảng Lỗi của định dạng; khi
    // lỗi, không trạng thái nào đổi.
    [[nodiscard]] Result<std::span<const ReceivedMessage>> read_packet(
        std::span<const std::byte> packet, core::MonoTime now) noexcept;

    // RTT làm mượt, từ các gói được xác nhận.
    [[nodiscard]] core::Duration rtt() const noexcept;
    // Số tin nhắn tin cậy chưa được xác nhận hết của một kênh tin cậy.
    [[nodiscard]] u32 in_flight(Channel channel) const noexcept;

    MessageChannels(MessageChannels&&) noexcept;
    MessageChannels& operator=(MessageChannels&&) noexcept;
    MessageChannels(const MessageChannels&) = delete;
    MessageChannels& operator=(const MessageChannels&) = delete;
    ~MessageChannels();

private:
    struct State;

    explicit MessageChannels(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace orion::net
