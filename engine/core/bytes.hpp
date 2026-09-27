#pragma once

// Đọc và ghi số theo thứ tự byte little-endian tường minh (CLAUDE.md X.7, X.10).
//
// Mọi định dạng ra đĩa hay ra mạng đi qua đây thay vì memcpy struct: kết quả không phụ thuộc thứ tự
// byte, padding hay căn lề của máy. ByteReader kiểm biên ở mọi lần đọc và trả Error khi dữ liệu
// thiếu, vì dữ liệu nó đọc luôn đến từ ngoài (X.5, X.9).
//
// Luồng: không đồng bộ; mỗi reader, writer thuộc một luồng.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <bit>
#include <concepts>
#include <cstddef>
#include <expected>
#include <span>
#include <type_traits>

namespace orion::core {

// Cùng vùng byte dưới dạng unsigned char, cho API C nhận unsigned char* (libsodium). std::byte và
// unsigned char có cùng biểu diễn, và đọc ghi mọi đối tượng qua kiểu ký tự là hợp lệ, nên phép đổi
// kiểu con trỏ này không có UB. Đây là nền của serializer, chỗ X.3 cho dùng reinterpret_cast; mọi
// module khác đi qua hai hàm này thay vì tự đổi kiểu.
[[nodiscard]] inline std::span<const unsigned char> as_uchars(
    const std::span<const std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): serializer (X.3), xem trên.
    return {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline std::span<unsigned char> as_writable_uchars(
    const std::span<std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): serializer (X.3), xem trên.
    return {reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()};
}

template <std::integral T>
[[nodiscard]] constexpr T load_le(const std::span<const std::byte, sizeof(T)> bytes) noexcept {
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        value = static_cast<U>(value | static_cast<U>(std::to_integer<U>(bytes[i]) << (8U * i)));
    }
    return static_cast<T>(value);
}

template <std::integral T>
constexpr void store_le(const std::span<std::byte, sizeof(T)> bytes, const T value) noexcept {
    using U = std::make_unsigned_t<T>;
    const auto bits = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<u8>((bits >> (8U * i)) & 0xFFU));
    }
}

// Đọc tuần tự từ một vùng byte, kiểm biên mọi lần đọc. Khi hết dữ liệu, lần đọc trả OutOfRange và
// vị trí không đổi, nên người gọi có thể báo lỗi chính xác chỗ hỏng.
class ByteReader {
public:
    explicit ByteReader(const std::span<const std::byte> data) noexcept : data_(data) {}

    template <std::integral T>
    [[nodiscard]] Result<T> read() noexcept {
        if (remaining() < sizeof(T)) {
            return fail(ErrorCode::OutOfRange, "ByteReader: thiếu dữ liệu",
                        static_cast<i64>(offset_));
        }
        const T value = load_le<T>(data_.subspan(offset_).first<sizeof(T)>());
        offset_ += sizeof(T);
        return value;
    }

    [[nodiscard]] Result<f32> read_f32() noexcept {
        const auto bits = read<u32>();
        if (!bits) {
            return std::unexpected(bits.error());
        }
        return std::bit_cast<f32>(*bits);
    }

    [[nodiscard]] Result<f64> read_f64() noexcept {
        const auto bits = read<u64>();
        if (!bits) {
            return std::unexpected(bits.error());
        }
        return std::bit_cast<f64>(*bits);
    }

    // `count` byte kế tiếp, trỏ thẳng vào vùng gốc (không sao chép); sống bằng vùng gốc.
    [[nodiscard]] Result<std::span<const std::byte>> read_bytes(const usize count) noexcept {
        if (remaining() < count) {
            return fail(ErrorCode::OutOfRange, "ByteReader: thiếu dữ liệu",
                        static_cast<i64>(offset_));
        }
        const auto bytes = data_.subspan(offset_, count);
        offset_ += count;
        return bytes;
    }

    [[nodiscard]] usize offset() const noexcept { return offset_; }
    [[nodiscard]] usize remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }

private:
    std::span<const std::byte> data_;
    usize offset_ = 0;
};

// Ghi tuần tự vào một vùng byte cố định. Ghi quá chỗ thì không ghi gì và bật cờ tràn; người gọi
// kiểm overflowed() một lần ở cuối thay vì sau mỗi lần ghi.
class ByteWriter {
public:
    explicit ByteWriter(const std::span<std::byte> data) noexcept : data_(data) {}

    template <std::integral T>
    void write(const T value) noexcept {
        if (!reserve(sizeof(T))) {
            return;
        }
        store_le<T>(data_.subspan(offset_).first<sizeof(T)>(), value);
        offset_ += sizeof(T);
    }

    void write_f32(const f32 value) noexcept { write(std::bit_cast<u32>(value)); }
    void write_f64(const f64 value) noexcept { write(std::bit_cast<u64>(value)); }

    void write_bytes(const std::span<const std::byte> bytes) noexcept {
        if (!reserve(bytes.size())) {
            return;
        }
        for (usize i = 0; i < bytes.size(); ++i) {
            data_[offset_ + i] = bytes[i];
        }
        offset_ += bytes.size();
    }

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] usize offset() const noexcept { return offset_; }
    // Phần đã ghi. Chỉ có nghĩa khi overflowed() là false.
    [[nodiscard]] std::span<const std::byte> written() const noexcept {
        return std::span<const std::byte>(data_).first(offset_);
    }

private:
    [[nodiscard]] bool reserve(const usize count) noexcept {
        if (overflowed_ || data_.size() - offset_ < count) {
            overflowed_ = true;
            return false;
        }
        return true;
    }

    std::span<std::byte> data_;
    usize offset_ = 0;
    bool overflowed_ = false;
};

}  // namespace orion::core
