#include "engine/net/bitstream.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"
#include "engine/math/scalar.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace orion::net {
namespace {

// Chỉ số lượng tử hoá lớn nhất: đủ cho mọi độ chính xác hợp lý, và mọi chỉ số đổi sang f64 đúng.
constexpr u64 kMaxQuantizationIndex = u64{1} << 32U;

[[nodiscard]] constexpr u64 low_mask(const u32 count) noexcept {
    return count >= 64 ? ~u64{0} : ((u64{1} << count) - 1);
}

}  // namespace

Quantization::Quantization(const f64 min, const f64 max, const f64 step) noexcept
    : min_(min), max_(max), step_(step) {
    constexpr f64 kHuge = std::numeric_limits<f64>::max();
    ORION_VERIFY(min > -kHuge && max < kHuge && min < max && step > 0 && step < kHuge,
                 "lượng tử hoá sai: [{}, {}] bước {}", min, max, step);
    const f64 steps = math::ceil((max - min) / step);
    ORION_VERIFY(steps <= static_cast<f64>(kMaxQuantizationIndex), "lượng tử hoá quá mịn: {} bước",
                 steps);
    max_index_ = static_cast<u64>(steps);
}

u64 Quantization::index_of(const f64 value) const noexcept {
    if (!(value > min_)) {
        return 0;  // gồm cả NaN
    }
    if (value >= max_) {
        return max_index_;
    }
    const auto index = static_cast<u64>(math::round((value - min_) / step_));
    return std::min(index, max_index_);
}

f64 Quantization::value_of(const u64 index) const noexcept {
    ORION_ASSERT(index <= max_index_, "chỉ số {} vượt {}", index, max_index_);
    const f64 value = min_ + (static_cast<f64>(index) * step_);
    return value > max_ ? max_ : value;
}

void BitWriter::write_bits(u64 value, u32 count) noexcept {
    ORION_ASSERT(count <= 64 && (value & ~low_mask(count)) == 0, "{} không vừa {} bit", value,
                 count);
    if (overflowed_ || count > (buffer_.size() * 8) - bit_position_) {
        overflowed_ = true;
        return;
    }
    while (count > 0) {
        const usize byte = bit_position_ / 8;
        const auto offset = static_cast<u32>(bit_position_ % 8);
        const u32 take = std::min(8U - offset, count);
        const auto part = static_cast<u8>((value & low_mask(take)) << offset);
        // Byte mới được gán, byte dở được OR, nên vùng nhớ không cần xoá trước.
        buffer_[byte] = offset == 0 ? std::byte{part} : buffer_[byte] | std::byte{part};
        value >>= take;
        count -= take;
        bit_position_ += take;
    }
}

void BitWriter::write_bool(const bool value) noexcept {
    write_bits(value ? 1U : 0U, 1);
}

void BitWriter::write_ranged(const i64 value, const i64 min, const i64 max) noexcept {
    ORION_ASSERT(min <= value && value <= max, "{} ngoài [{}, {}]", value, min, max);
    // Hiệu tính trên u64 (quay vòng theo modulo 2^64), nên đúng cả khi khoảng rộng hơn i64.
    const u64 range = static_cast<u64>(max) - static_cast<u64>(min);
    write_bits(static_cast<u64>(value) - static_cast<u64>(min), bits_for(range));
}

void BitWriter::write_quantized(const f64 value, const Quantization& quantization) noexcept {
    write_bits(quantization.index_of(value), quantization.bits());
}

void BitWriter::write_bytes(const std::span<const std::byte> bytes, const usize max_size) noexcept {
    ORION_ASSERT(bytes.size() <= max_size, "{} byte vượt {}", bytes.size(), max_size);
    write_bits(bytes.size(), bits_for(max_size));
    for (const std::byte b : bytes) {
        write_bits(std::to_integer<u64>(b), 8);
    }
}

void BitWriter::write_string(const std::string_view text, const usize max_size) noexcept {
    ORION_ASSERT(core::is_valid_utf8(text), "chuỗi không phải UTF-8");
    write_bytes(std::as_bytes(std::span(text)), max_size);
}

Result<u64> BitReader::read_bits(const u32 count) noexcept {
    ORION_ASSERT(count <= 64, "đọc {} bit", count);
    if (count > bits_remaining()) {
        return fail(ErrorCode::OutOfRange, "bitstream: thiếu bit", static_cast<i64>(count));
    }
    u64 value = 0;
    u32 done = 0;
    while (done < count) {
        const usize byte = bit_position_ / 8;
        const auto offset = static_cast<u32>(bit_position_ % 8);
        const u32 take = std::min(8U - offset, count - done);
        const u64 part = (std::to_integer<u64>(data_[byte]) >> offset) & low_mask(take);
        value |= part << done;
        done += take;
        bit_position_ += take;
    }
    return value;
}

Result<bool> BitReader::read_bool() noexcept {
    const Result<u64> bit = read_bits(1);
    if (!bit) {
        return std::unexpected(bit.error());
    }
    return *bit != 0;
}

Result<i64> BitReader::read_ranged(const i64 min, const i64 max) noexcept {
    ORION_ASSERT(min <= max, "khoảng [{}, {}] rỗng", min, max);
    const u64 range = static_cast<u64>(max) - static_cast<u64>(min);
    const usize start = bit_position_;
    const Result<u64> offset = read_bits(bits_for(range));
    if (!offset) {
        return std::unexpected(offset.error());
    }
    if (*offset > range) {
        bit_position_ = start;
        return fail(ErrorCode::InvalidArgument, "bitstream: số vượt khoảng đã khai");
    }
    return static_cast<i64>(static_cast<u64>(min) + *offset);
}

Result<f64> BitReader::read_quantized(const Quantization& quantization) noexcept {
    const usize start = bit_position_;
    const Result<u64> index = read_bits(quantization.bits());
    if (!index) {
        return std::unexpected(index.error());
    }
    if (*index > quantization.max_index()) {
        bit_position_ = start;
        return fail(ErrorCode::InvalidArgument, "bitstream: chỉ số lượng tử hoá vượt khoảng");
    }
    return quantization.value_of(*index);
}

Result<usize> BitReader::read_bytes(const std::span<std::byte> out, const usize max_size) noexcept {
    ORION_ASSERT(out.size() >= max_size, "bộ đệm {} byte nhỏ hơn {}", out.size(), max_size);
    const usize start = bit_position_;
    const Result<u64> size = read_bits(bits_for(max_size));
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > max_size) {
        bit_position_ = start;
        return fail(ErrorCode::InvalidArgument, "bitstream: độ dài vượt giới hạn đã khai");
    }
    if (*size * 8 > bits_remaining()) {
        bit_position_ = start;
        return fail(ErrorCode::OutOfRange, "bitstream: thiếu byte", static_cast<i64>(*size));
    }
    for (usize i = 0; i < *size; ++i) {
        out[i] = static_cast<std::byte>(*read_bits(8));
    }
    return static_cast<usize>(*size);
}

Result<usize> BitReader::read_string(const std::span<char> out, const usize max_size) noexcept {
    const usize start = bit_position_;
    const Result<usize> size = read_bytes(std::as_writable_bytes(out), max_size);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (!core::is_valid_utf8(std::string_view(out.data(), *size))) {
        bit_position_ = start;
        return fail(ErrorCode::InvalidArgument, "bitstream: chuỗi không phải UTF-8");
    }
    return *size;
}

Result<void> BitReader::finish() const noexcept {
    const usize used = (bit_position_ + 7) / 8;
    if (used != data_.size()) {
        return fail(ErrorCode::InvalidArgument, "bitstream: còn byte thừa sau tin nhắn",
                    static_cast<i64>(data_.size() - used));
    }
    const auto offset = static_cast<u32>(bit_position_ % 8);
    if (offset != 0 && (std::to_integer<u32>(data_[used - 1]) >> offset) != 0) {
        return fail(ErrorCode::InvalidArgument, "bitstream: bit đệm khác 0");
    }
    return {};
}

}  // namespace orion::net
