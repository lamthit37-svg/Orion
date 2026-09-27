#include "game/shared/protocol/codec.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/bitstream.hpp"

#include <expected>

namespace orion::protocol {

void Encoder::write_bool(const bool value) noexcept {
    if (ok()) {
        writer_.write_bool(value);
    }
}

void Encoder::write_bits(const u64 value, const u32 count) noexcept {
    ORION_ASSERT(count >= 1 && count <= 64, "{} bit", count);
    if (count < 64 && (value >> count) != 0) {
        reject();
        return;
    }
    if (ok()) {
        writer_.write_bits(value, count);
    }
}

void Encoder::write_quantized(const f64 value, const net::Quantization& quantization) noexcept {
    if (ok()) {
        writer_.write_quantized(value, quantization);
    }
}

void Encoder::write_length(const usize length, const usize max) noexcept {
    ORION_ASSERT(length <= max, "độ dài {} vượt {}", length, max);
    if (ok()) {
        writer_.write_bits(length, net::bits_for(max));
    }
}

void Encoder::write_tick(const Tick tick) noexcept {
    write_bits(tick.value, 64);
}

void Encoder::write_short_tick(const ShortTick tick) noexcept {
    write_bits(tick.value, 16);
}

void Encoder::write_replicated_id(const ReplicatedId id) noexcept {
    write_bits(id.value, 32);
}

void Encoder::reject() noexcept {
    invalid_ = true;
}

Result<usize> Encoder::finish() const noexcept {
    if (invalid_) {
        return fail(ErrorCode::InvalidArgument, "protocol: trường ngoài khoảng đã khai");
    }
    if (writer_.overflowed()) {
        return fail(ErrorCode::ResourceExhausted, "protocol: bộ đệm nhỏ hơn tin nhắn");
    }
    return writer_.byte_count();
}

void Decoder::read_bool(bool& out) noexcept {
    if (!ok()) {
        return;
    }
    const Result<bool> value = reader_.read_bool();
    if (!value) {
        error_ = value.error();
        return;
    }
    out = *value;
}

void Decoder::read_quantized(f64& out, const net::Quantization& quantization) noexcept {
    if (!ok()) {
        return;
    }
    const Result<f64> value = reader_.read_quantized(quantization);
    if (!value) {
        error_ = value.error();
        return;
    }
    out = *value;
}

usize Decoder::read_length(const usize max) noexcept {
    usize length = 0;
    read_int(length, 0, static_cast<i64>(max));
    return length;
}

void Decoder::read_tick(Tick& out) noexcept {
    read_bits(out.value, 64);
}

void Decoder::read_short_tick(ShortTick& out) noexcept {
    read_bits(out.value, 16);
}

void Decoder::read_replicated_id(ReplicatedId& out) noexcept {
    read_bits(out.value, 32);
}

void Decoder::reject() noexcept {
    if (ok()) {
        error_ = Error(ErrorCode::InvalidArgument, "protocol: giá trị không có trong schema");
    }
}

Result<void> Decoder::finish() const noexcept {
    if (error_.has_value()) {
        return std::unexpected(*error_);
    }
    return reader_.finish();
}

}  // namespace orion::protocol
