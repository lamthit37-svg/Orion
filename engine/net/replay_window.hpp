#pragma once

// Cửa sổ chống replay (docs/formats/transport.md, mục Cửa sổ chống replay; CLAUDE.md X.9): nhớ số
// thứ tự lớn nhất đã nhận và kSize số ngay trước nó, theo cách của RFC 6479. Không cấp phát; mỗi
// kết nối giữ một cửa sổ cho chiều nhận của nó. Không đồng bộ: thuộc luồng xử lý kết nối đó.

#include "engine/core/types.hpp"

#include <array>
#include <optional>

namespace orion::net {

class ReplayWindow {
public:
    static constexpr u64 kSize = 1024;

    // true khi `sequence` chưa được ghi nhận và không cũ hơn cửa sổ. Không đổi trạng thái: hỏi
    // trước khi giải mã (bước 3 của mục Nhận).
    [[nodiscard]] bool fresh(u64 sequence) const noexcept;
    // Ghi nhận `sequence` của một gói đã qua AEAD (bước 4). Số không fresh thì không đổi gì.
    void record(u64 sequence) noexcept;
    // Số lớn nhất đã ghi nhận; nullopt khi chưa có gói nào.
    [[nodiscard]] std::optional<u64> highest() const noexcept;

private:
    static constexpr u64 kWordBits = 64;

    [[nodiscard]] bool seen(u64 sequence) const noexcept;
    void set(u64 sequence, bool value) noexcept;

    // Bit của số s nằm ở vị trí s mod kSize; chỉ có nghĩa cho các số trong cửa sổ.
    std::array<u64, kSize / kWordBits> bits_{};
    u64 highest_ = 0;
    bool empty_ = true;
};

}  // namespace orion::net
