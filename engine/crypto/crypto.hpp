#pragma once

// Crypto của Orion (ARCH §4.1, ADR 0011): bọc libsodium, không lộ kiểu nào của nó ra header (X.2).
// Không tự chế thuật toán (X.9); mọi thuật toán là của libsodium.
//
// initialize() phải thành công trước mọi hàm khác của module; gọi một lần trong main, gọi lại vô
// hại và an toàn giữa các luồng. Sau đó mọi hàm dùng được từ nhiều luồng cùng lúc.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>

namespace orion::crypto {

[[nodiscard]] Result<void> initialize() noexcept;

// Byte ngẫu nhiên từ CSPRNG của hệ điều hành, cho khoá và salt. Mô phỏng không dùng hàm này mà dùng
// PRNG có seed của engine/math (X.11).
void random_bytes(std::span<std::byte> out) noexcept;

// So sánh thời gian hằng (X.9): thời gian chỉ phụ thuộc độ dài. Độ dài không phải bí mật, nên hai
// span khác độ dài trả false ngay.
[[nodiscard]] bool equal_constant_time(std::span<const std::byte> a,
                                       std::span<const std::byte> b) noexcept;

// Ghi 0 lên vùng nhớ theo cách compiler không được bỏ qua.
void secure_zero(std::span<std::byte> data) noexcept;

// Dữ liệu công khai cỡ cố định: khoá công khai, chữ ký, hash, nonce. Tag phân biệt các loại để
// không truyền nhầm khoá này vào chỗ khoá kia. Không có operator==: mọi so sánh đi qua
// equal_constant_time, kể cả với hash và chữ ký (X.9).
template <usize N, class Tag>
struct PublicBytes {
    static constexpr usize kSize = N;

    std::array<std::byte, N> bytes{};

    [[nodiscard]] std::span<const std::byte, N> view() const noexcept { return bytes; }
};

// Bí mật cỡ cố định: khoá bí mật, khoá phiên, seed. Tự ghi 0 khi huỷ và khi bị move đi. Không có
// operator== và không định dạng được, nên không lọt vào log (X.5).
template <usize N, class Tag>
class SecretBytes {
public:
    static constexpr usize kSize = N;

    SecretBytes() = default;
    explicit SecretBytes(const std::span<const std::byte, N> bytes) noexcept {
        std::ranges::copy(bytes, bytes_.begin());
    }
    SecretBytes(const SecretBytes&) = default;
    SecretBytes& operator=(const SecretBytes&) = default;
    SecretBytes(SecretBytes&& other) noexcept : bytes_(other.bytes_) { secure_zero(other.bytes_); }
    SecretBytes& operator=(SecretBytes&& other) noexcept {
        if (this != &other) {
            bytes_ = other.bytes_;
            secure_zero(other.bytes_);
        }
        return *this;
    }
    ~SecretBytes() { secure_zero(bytes_); }

    [[nodiscard]] std::span<const std::byte, N> view() const noexcept { return bytes_; }
    [[nodiscard]] std::span<std::byte, N> mutable_view() noexcept { return bytes_; }

private:
    std::array<std::byte, N> bytes_{};
};

template <usize N, class Tag>
[[nodiscard]] bool equal_constant_time(const PublicBytes<N, Tag>& a,
                                       const PublicBytes<N, Tag>& b) noexcept {
    return equal_constant_time(a.view(), b.view());
}

template <usize N, class Tag>
[[nodiscard]] bool equal_constant_time(const SecretBytes<N, Tag>& a,
                                       const SecretBytes<N, Tag>& b) noexcept {
    return equal_constant_time(a.view(), b.view());
}

}  // namespace orion::crypto
