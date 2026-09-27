#pragma once

// Phía server của transport (docs/formats/transport.md, mục Bắt tay và Kết nối): gateway đưa mọi
// gói UDP nhận được vào receive, và gửi các gói ServerTransport ghi vào Outbox. Lớp này không chạm
// socket và nhận thời gian từ bên gọi (đồng hồ đơn điệu cho cookie và hạn kết nối, đồng hồ tường
// cho hạn của token), nên test chạy tất định trên mạng giả (X.4).
//
// - Không gói nào tới tầng trên trước khi xác thực (X.9): gói bắt tay chỉ sinh gói trả lời nhỏ hơn
//   nó (chống khuếch đại); gói dữ liệu chỉ tới tầng trên sau khi qua cửa sổ chống replay và AEAD.
// - Đường gói dữ liệu (receive, send, update) không cấp phát (X.7). Mỗi kết nối mới thêm một nút
//   vào bảng token đã dùng; bảng kết nối và bảng giới hạn theo địa chỉ cấp một lần lúc create.
// - Mọi loại gói nhận có giới hạn tần suất (X.9; docs/formats/transport.md, mục Giới hạn tần suất):
//   REQUEST cho cả server, RESPONSE có cookie đúng theo địa chỉ nguồn và cho cả server, gói dữ
//   liệu đã xác thực theo kết nối. Gói vượt giới hạn bị bỏ, không trả lời gì.
// - Không đồng bộ: thuộc luồng IO mạng của gateway (ARCH §7).

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/short_hash.hpp"
#include "engine/crypto/sign.hpp"
#include "engine/net/address.hpp"
#include "engine/net/connection.hpp"
#include "engine/net/outbox.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/rate_limit.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace orion::net {

// Giới hạn tần suất mặc định (docs/formats/transport.md, mục Giới hạn tần suất). Chính sách, không
// phải số đo.
inline constexpr RateLimit kRequestLimit = RateLimit::per_second(10'000, 20'000);
inline constexpr RateLimit kResponseLimitPerAddress = RateLimit::per_second(20, 40);
inline constexpr RateLimit kResponseLimit = RateLimit::per_second(2'000, 4'000);
inline constexpr RateLimit kPacketLimitPerConnection = RateLimit::per_second(200, 400);
inline constexpr usize kAddressLimiterSlots = 4'096;
inline constexpr usize kMaxAddressLimiterSlots = usize{1} << 20U;

struct ServerConfig {
    // Khoá định danh của cụm server: ký ACCEPT; token phải ghi đúng khoá công khai này.
    crypto::SigningKeyPair identity;
    // Khoá công khai của auth; token phải do một trong số đó ký (danh sách để xoay khoá).
    std::vector<crypto::SigningPublicKey> token_signers;
    // kProtocolVersion của schema (ADR 0004).
    u32 protocol_version = 0;
    // Số kết nối tối đa, tính cả kết nối chờ; ít nhất 1.
    u32 max_connections = 0;
    // Giới hạn tần suất; mỗi cái phải valid().
    RateLimit request_limit = kRequestLimit;
    RateLimit response_limit_per_address = kResponseLimitPerAddress;
    RateLimit response_limit = kResponseLimit;
    RateLimit packet_limit_per_connection = kPacketLimitPerConnection;
    // Số ô của bảng giới hạn RESPONSE theo địa chỉ, 1 tới kMaxAddressLimiterSlots.
    usize address_slots = kAddressLimiterSlots;
    // Khoá SipHash của bảng đó; không đặt thì create sinh khoá ngẫu nhiên. Test và fuzz đặt khoá cố
    // định để kết quả tất định (X.4).
    std::optional<crypto::ShortHashKey> address_hash_key = std::nullopt;
};

// Điều một gói vừa nhận thay đổi ở tầng trên.
struct ServerEvent {
    // 0 khi gói không tới tầng trên (gói bắt tay, gói hỏng, gói lặp lại).
    u32 connection_id = 0;
    // Gói này xác nhận kết nối: từ giờ tầng trên gửi được tới connection_id.
    bool connected = false;
    // Bên kia ngắt kết nối; connection_id không còn dùng được.
    bool disconnected = false;
    // Id tài khoản trong token của kết nối (ADR 0006).
    u64 account_id = 0;
    // Dữ liệu tầng trên trong gói; trỏ vào bộ đệm của ServerTransport, chỉ sống tới lần gọi sau.
    std::span<const std::byte> payload;
};

class ServerTransport {
public:
    // Lỗi InvalidArgument khi max_connections là 0, danh sách khoá của auth rỗng, một giới hạn tần
    // suất không valid() hay address_slots ngoài khoảng. crypto::initialize phải thành công trước
    // đó.
    [[nodiscard]] static Result<ServerTransport> create(ServerConfig config, core::MonoTime now);

    // Xử lý một gói nhận từ `from`. Gói trả lời (CHALLENGE, ACCEPT, REJECT) được ghi vào `outbox`.
    [[nodiscard]] ServerEvent receive(const Address& from, std::span<const std::byte> packet,
                                      core::MonoTime now, core::WallTime wall_now,
                                      Outbox& outbox) noexcept;

    // Gửi dữ liệu tầng trên (tối đa kMaxTransportPayload byte) tới một kết nối đã xác nhận. Lỗi:
    // NotFound (không có kết nối, hay chưa xác nhận), InvalidArgument (quá lớn hay rỗng),
    // ResourceExhausted (hết số thứ tự: kết nối bị đóng).
    [[nodiscard]] Result<void> send(u32 connection_id, std::span<const std::byte> payload,
                                    core::MonoTime now, Outbox& outbox) noexcept;

    // Gửi kDisconnectRepeats gói ngắt rồi bỏ kết nối. Không có kết nối thì không làm gì.
    void disconnect(u32 connection_id, Outbox& outbox) noexcept;

    // Bỏ kết nối chờ quá kPendingTimeout và kết nối im lặng quá kIdleTimeout, gửi keep-alive, quên
    // token đã hết hạn. Id của kết nối đã xác nhận bị bỏ được ghi vào `closed`; trả số đã ghi. Khi
    // `closed` đầy, phần còn lại được bỏ ở lần gọi sau.
    [[nodiscard]] usize update(core::MonoTime now, core::WallTime wall_now, Outbox& outbox,
                               std::span<u32> closed) noexcept;

    [[nodiscard]] u32 connection_count() const noexcept;
    // Địa chỉ hiện tại của một kết nối (đổi khi client đổi mạng); nullopt khi không có.
    [[nodiscard]] std::optional<Address> address_of(u32 connection_id) const noexcept;

    ServerTransport(ServerTransport&&) noexcept;
    ServerTransport& operator=(ServerTransport&&) noexcept;
    ServerTransport(const ServerTransport&) = delete;
    ServerTransport& operator=(const ServerTransport&) = delete;
    ~ServerTransport();

private:
    // Bảng kết nối, bảng token đã dùng, cookie jar và bộ đệm: ở một chỗ cố định trên heap, để
    // payload của ServerEvent vẫn đúng khi ServerTransport bị chuyển đi.
    struct State;

    explicit ServerTransport(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace orion::net
