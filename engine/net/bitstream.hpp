#pragma once

// Ghi và đọc theo bit cho gói tin (ADR 0004; docs/formats/protocol.md, mục "Bitstream"). Bit đầu
// tiên ghi vào là bit thấp nhất của byte đầu tiên, và số nhiều bit ghi bit thấp trước. Code sinh ra
// từ game/shared/protocol gọi các hàm này.
//
// - BitWriter ghi vào vùng nhớ có sẵn, không cấp phát. Ghi quá chỗ thì không ghi gì nữa và bật cờ
//   tràn, như core::ByteWriter: bên gọi kiểm overflowed() một lần ở cuối. Giá trị vào do chính tiến
//   trình tạo ra, nên giá trị ngoài khoảng đã khai là lỗi lập trình (ORION_ASSERT).
// - BitReader là parser của dữ liệu từ mạng (X.5, X.9): mọi lần đọc kiểm biên và trả Result.
//   finish() buộc phần đệm của byte cuối toàn 0 và không có byte thừa, nên mỗi tin nhắn có đúng một
//   cách mã hoá.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <bit>
#include <cstddef>
#include <span>
#include <string_view>

namespace orion::net {

// Số bit để ghi mọi giá trị từ 0 tới `max_value`; 0 khi max_value = 0 (chỉ có một giá trị, không
// cần ghi gì).
[[nodiscard]] constexpr u32 bits_for(const u64 max_value) noexcept {
    return static_cast<u32>(std::bit_width(max_value));
}

// Lượng tử hoá số thực trong [min, max] theo bước `step`: chỉ số 0..max_index với
// max_index = ceil((max - min) / step), và giá trị của chỉ số i là min + i · step, kẹp vào max.
// Phép tính chỉ dùng phép IEEE làm tròn đúng, nên cùng kết quả trên mọi toolchain (X.11).
class Quantization {
public:
    // min < max, step > 0, max_index tới 2^32; mọi số hữu hạn. Schema do codegen kiểm trước, nên
    // sai ở đây là lỗi lập trình.
    Quantization(f64 min, f64 max, f64 step) noexcept;

    [[nodiscard]] f64 min() const noexcept { return min_; }
    [[nodiscard]] f64 max() const noexcept { return max_; }
    [[nodiscard]] f64 step() const noexcept { return step_; }
    [[nodiscard]] u64 max_index() const noexcept { return max_index_; }
    [[nodiscard]] u32 bits() const noexcept { return bits_for(max_index_); }

    // Chỉ số gần nhất (làm tròn nửa ra xa 0) của value sau khi kẹp vào [min, max]; NaN cho 0, tức
    // min.
    [[nodiscard]] u64 index_of(f64 value) const noexcept;
    // Giá trị của chỉ số (index <= max_index).
    [[nodiscard]] f64 value_of(u64 index) const noexcept;

private:
    f64 min_;
    f64 max_;
    f64 step_;
    u64 max_index_ = 0;
};

class BitWriter {
public:
    explicit BitWriter(std::span<std::byte> buffer) noexcept : buffer_(buffer) {}

    // `count` bit thấp của value, count từ 0 tới 64; value < 2^count.
    void write_bits(u64 value, u32 count) noexcept;
    void write_bool(bool value) noexcept;
    // value - min trên bits_for(max - min) bit; min <= value <= max.
    void write_ranged(i64 value, i64 min, i64 max) noexcept;
    void write_quantized(f64 value, const Quantization& quantization) noexcept;
    // Độ dài trên bits_for(max_size) bit rồi từng byte, 8 bit mỗi byte; bytes.size() <= max_size.
    void write_bytes(std::span<const std::byte> bytes, usize max_size) noexcept;
    // Như write_bytes; text là UTF-8 hợp lệ.
    void write_string(std::string_view text, usize max_size) noexcept;

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] usize bit_count() const noexcept { return bit_position_; }
    // Số byte đã dùng, làm tròn lên; phần đệm của byte cuối là 0.
    [[nodiscard]] usize byte_count() const noexcept { return (bit_position_ + 7) / 8; }
    // Phần đã ghi. Chỉ có nghĩa khi overflowed() là false.
    [[nodiscard]] std::span<const std::byte> written() const noexcept {
        return buffer_.first(byte_count());
    }

private:
    std::span<std::byte> buffer_;
    usize bit_position_ = 0;
    bool overflowed_ = false;
};

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) noexcept : data_(data) {}

    // Lỗi của mọi hàm đọc: OutOfRange khi không đủ bit; InvalidArgument khi giá trị vượt khoảng đã
    // khai hay chuỗi không phải UTF-8. Khi lỗi, vị trí đọc không đổi.
    [[nodiscard]] Result<u64> read_bits(u32 count) noexcept;
    [[nodiscard]] Result<bool> read_bool() noexcept;
    [[nodiscard]] Result<i64> read_ranged(i64 min, i64 max) noexcept;
    [[nodiscard]] Result<f64> read_quantized(const Quantization& quantization) noexcept;
    // Chép vào đầu `out` (out.size() >= max_size) và trả độ dài.
    [[nodiscard]] Result<usize> read_bytes(std::span<std::byte> out, usize max_size) noexcept;
    [[nodiscard]] Result<usize> read_string(std::span<char> out, usize max_size) noexcept;
    // Kết thúc tin nhắn: các bit còn lại của byte hiện tại là 0 và không còn byte nào sau nó. Lỗi:
    // InvalidArgument.
    [[nodiscard]] Result<void> finish() const noexcept;

    [[nodiscard]] usize bits_remaining() const noexcept {
        return (data_.size() * 8) - bit_position_;
    }

private:
    std::span<const std::byte> data_;
    usize bit_position_ = 0;
};

}  // namespace orion::net
