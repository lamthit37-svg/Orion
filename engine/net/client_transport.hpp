#pragma once

// Phía client của transport (docs/formats/transport.md, mục Bắt tay và Kết nối): bắt tay với
// gateway rồi gửi, nhận dữ liệu tầng trên. Như ServerTransport, lớp này không chạm socket và nhận
// thời gian từ bên gọi (X.4); vòng lặp của client đưa gói nhận được vào receive, gọi update mỗi
// frame, và gửi các gói trong Outbox. Không cấp phát sau connect (X.7). Không đồng bộ.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connect_token.hpp"
#include "engine/net/handshake.hpp"
#include "engine/net/outbox.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace orion::net {

enum class ClientState : u8 {
    Connecting,
    Connected,
    Closed,
};

enum class CloseReason : u8 {
    None,
    // Server trả REJECT; lý do ở reject_reason().
    Rejected,
    // Không xong bắt tay trong kHandshakeTimeout.
    HandshakeTimeout,
    // Không nhận gói hợp lệ nào trong kIdleTimeout.
    Timeout,
    ServerDisconnected,
    LocalDisconnect,
    // Khoá X25519 của server trong ACCEPT (đã ký đúng) không trao được khoá.
    KeyExchangeFailed,
    // Đã dùng hết 2^64 số thứ tự.
    SequenceExhausted,
};

// Điều một lần gọi receive hay update thay đổi ở tầng trên.
struct ClientEvent {
    // Bắt tay vừa xong: từ giờ send dùng được.
    bool connected = false;
    // Kết nối vừa đóng; lý do ở close_reason().
    bool closed = false;
    // Dữ liệu tầng trên trong gói; trỏ vào bộ đệm của ClientTransport, chỉ sống tới lần gọi sau.
    std::span<const std::byte> payload;
};

class ClientTransport {
public:
    // Bắt đầu bắt tay với `server` và ghi REQUEST đầu tiên vào `outbox`. `key` là cặp khoá X25519
    // mà client gửi khoá công khai cho auth lúc xin token. Lỗi: như inspect_connect_token, và
    // InvalidArgument khi khoá công khai của `key` khác client_key trong token.
    // crypto::initialize phải thành công trước đó.
    [[nodiscard]] static Result<ClientTransport> connect(const Address& server,
                                                         const ConnectToken& token,
                                                         const crypto::KeyExchangeKeyPair& key,
                                                         u32 protocol_version, core::MonoTime now,
                                                         Outbox& outbox);

    // Xử lý một gói nhận từ `from`; gói không từ địa chỉ của server bị bỏ.
    [[nodiscard]] ClientEvent receive(const Address& from, std::span<const std::byte> packet,
                                      core::MonoTime now, Outbox& outbox) noexcept;
    // Gửi dữ liệu tầng trên (1 tới kMaxTransportPayload byte). Lỗi: FailedPrecondition (chưa kết
    // nối hay đã đóng), InvalidArgument (sai cỡ), ResourceExhausted (hết số thứ tự: kết nối đóng).
    [[nodiscard]] Result<void> send(std::span<const std::byte> payload, core::MonoTime now,
                                    Outbox& outbox) noexcept;
    // Đóng kết nối; đang kết nối thì gửi kDisconnectRepeats gói ngắt trước.
    void disconnect(Outbox& outbox) noexcept;
    // Gửi lại gói bắt tay, gửi keep-alive, và đóng khi quá hạn.
    [[nodiscard]] ClientEvent update(core::MonoTime now, Outbox& outbox) noexcept;

    [[nodiscard]] ClientState state() const noexcept;
    [[nodiscard]] CloseReason close_reason() const noexcept;
    // Lý do trong REJECT khi close_reason() là Rejected.
    [[nodiscard]] std::optional<RejectReason> reject_reason() const noexcept;
    // Connection id do server cấp; 0 khi chưa kết nối.
    [[nodiscard]] u32 connection_id() const noexcept;

    ClientTransport(ClientTransport&&) noexcept;
    ClientTransport& operator=(ClientTransport&&) noexcept;
    ClientTransport(const ClientTransport&) = delete;
    ClientTransport& operator=(const ClientTransport&) = delete;
    ~ClientTransport();

private:
    struct State;

    explicit ClientTransport(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace orion::net
