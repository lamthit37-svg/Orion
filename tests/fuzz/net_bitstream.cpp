// Fuzz BitReader (CLAUDE.md X.4, X.9; docs/formats/protocol.md, mục "Bitstream"): gói tin là dữ
// liệu từ ngoài. Input tự mô tả một dãy trường: mỗi trường mở đầu bằng 4 bit loại rồi tới giá trị;
// loại 7 kết thúc, và finish() phải nhận phần còn lại; loại lạ dừng mà không kiểm gì thêm. Mọi
// input cho ra một lỗi trong bảng hoặc một dãy trường. Dãy được nhận thì ghi lại bằng BitWriter
// phải ra đúng từng byte của input: mỗi tin nhắn chỉ có một cách mã hoá.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/net/bitstream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using orion::ErrorCode;
using orion::i64;
using orion::Result;
using orion::u64;
using orion::usize;
using orion::net::BitReader;
using orion::net::BitWriter;
using orion::net::Quantization;

constexpr usize kMaxBytes = 20;

// Lỗi đọc hợp lệ là lỗi trong bảng của định dạng; trả false để harness dừng.
template <class T>
[[nodiscard]] bool accepted(const Result<T>& result) {
    if (result.has_value()) {
        return true;
    }
    const ErrorCode code = result.error().code();
    ORION_VERIFY(code == ErrorCode::OutOfRange || code == ErrorCode::InvalidArgument,
                 "mã lỗi ngoài bảng của định dạng");
    return false;
}

[[nodiscard]] bool copy_bits(BitReader& reader, BitWriter& writer) {
    const Result<u64> width = reader.read_bits(6);
    if (!accepted(width)) {
        return false;
    }
    const auto bits = static_cast<orion::u32>(*width + 1);
    const Result<u64> value = reader.read_bits(bits);
    if (!accepted(value)) {
        return false;
    }
    writer.write_bits(*width, 6);
    writer.write_bits(*value, bits);
    return true;
}

[[nodiscard]] bool copy_ranged(const i64 min, const i64 max, BitReader& reader, BitWriter& writer) {
    const Result<i64> value = reader.read_ranged(min, max);
    if (!accepted(value)) {
        return false;
    }
    writer.write_ranged(*value, min, max);
    return true;
}

[[nodiscard]] bool copy_quantized(const Quantization& quantization, BitReader& reader,
                                  BitWriter& writer) {
    const Result<orion::f64> value = reader.read_quantized(quantization);
    if (!accepted(value)) {
        return false;
    }
    writer.write_quantized(*value, quantization);
    return true;
}

[[nodiscard]] bool copy_bytes(BitReader& reader, BitWriter& writer) {
    std::array<std::byte, kMaxBytes> bytes{};
    const Result<usize> size = reader.read_bytes(bytes, kMaxBytes);
    if (!accepted(size)) {
        return false;
    }
    writer.write_bytes(std::span(bytes).first(*size), kMaxBytes);
    return true;
}

[[nodiscard]] bool copy_string(BitReader& reader, BitWriter& writer) {
    std::array<char, kMaxBytes> text{};
    const Result<usize> size = reader.read_string(text, kMaxBytes);
    if (!accepted(size)) {
        return false;
    }
    writer.write_string(std::string_view(text.data(), *size), kMaxBytes);
    return true;
}

// Đọc một trường loại `kind` rồi ghi lại nó; false khi đọc lỗi hay loại lạ.
[[nodiscard]] bool copy_field(const u64 kind, BitReader& reader, BitWriter& writer) {
    static const Quantization kCentimetres(-10.0, 10.0, 0.01);
    static const Quantization kUneven(0.0, 1.0, 0.3);
    switch (kind) {
        case 0: {
            const Result<bool> value = reader.read_bool();
            if (!accepted(value)) {
                return false;
            }
            writer.write_bool(*value);
            return true;
        }
        case 1:
            return copy_bits(reader, writer);
        case 2:
            return copy_ranged(-1000, 1000, reader, writer);
        case 3:
            return copy_ranged(std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(),
                               reader, writer);
        case 4:
            return copy_quantized(kCentimetres, reader, writer);
        case 5:
            return copy_bytes(reader, writer);
        case 6:
            return copy_string(reader, writer);
        case 8:
            return copy_quantized(kUneven, reader, writer);
        default:
            return false;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size);
    if (size > 0) {
        std::memcpy(input.data(), data, size);
    }
    BitReader reader(input);
    std::vector<std::byte> output(size + 16);
    BitWriter writer(output);
    while (true) {
        const Result<u64> kind = reader.read_bits(4);
        if (!accepted(kind)) {
            return 0;
        }
        writer.write_bits(*kind, 4);
        if (*kind == 7) {
            const Result<void> finished = reader.finish();
            if (!finished.has_value()) {
                ORION_VERIFY(finished.error().code() == ErrorCode::InvalidArgument,
                             "mã lỗi ngoài bảng của định dạng");
                return 0;
            }
            ORION_VERIFY(!writer.overflowed() && std::ranges::equal(writer.written(), input),
                         "ghi lại không ra đúng input: có hai cách mã hoá");
            return 0;
        }
        if (!copy_field(*kind, reader, writer)) {
            return 0;
        }
    }
}
