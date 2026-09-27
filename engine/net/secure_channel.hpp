#pragma once

// Trạng thái mã hoá của một kết nối đã trao khoá (docs/formats/transport.md, mục Gói dữ liệu): khoá
// hai chiều, số thứ tự gửi kế tiếp, cửa sổ chống replay của chiều nhận. Client và server mỗi bên
// giữ một kênh cho mỗi kết nối. Không cấp phát; không đồng bộ.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/key_exchange.hpp"
#include "engine/net/packet.hpp"
#include "engine/net/replay_window.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace orion::net {

class SecureChannel {
public:
    // `first_sequence` là số thứ tự của gói gửi đầu tiên. Định dạng đánh số từ 0; số khác chỉ để
    // test chạm được biên 2^64 − 1.
    SecureChannel(u32 connection_id, crypto::SessionKeys keys, u64 first_sequence = 0) noexcept;

    [[nodiscard]] u32 connection_id() const noexcept { return connection_id_; }

    // Niêm phong `payload` thành gói dữ liệu với số thứ tự kế tiếp, ghi vào đầu `out`, trả cỡ gói.
    // Lỗi: InvalidArgument như seal_data_packet (khi đó số thứ tự không bị tiêu), ResourceExhausted
    // khi đã dùng hết 2^64 số thứ tự: kết nối phải đóng.
    [[nodiscard]] Result<usize> seal(std::span<std::byte> out,
                                     std::span<const std::byte> payload) noexcept;

    // Mở một gói dữ liệu của kết nối này theo mục Nhận của định dạng: hỏi cửa sổ, mở AEAD, rồi mới
    // ghi số thứ tự vào cửa sổ. Ghi payload vào đầu `out`, trả số byte. Lỗi: InvalidArgument
    // (header sai, gói của kết nối khác, `out` thiếu chỗ), AlreadyExists (số thứ tự đã nhận hay cũ
    // hơn cửa sổ), DataLoss (tag sai). Khi lỗi, cửa sổ không đổi.
    [[nodiscard]] Result<usize> open(std::span<std::byte> out,
                                     std::span<const std::byte> packet) noexcept;

    // Số thứ tự lớn nhất đã nhận hợp lệ; nullopt khi chưa nhận gói nào.
    [[nodiscard]] std::optional<u64> highest_received() const noexcept { return window_.highest(); }

private:
    u32 connection_id_;
    crypto::SessionKeys keys_;
    u64 next_sequence_ = 0;
    bool exhausted_ = false;
    ReplayWindow window_;
};

}  // namespace orion::net
