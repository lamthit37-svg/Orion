#pragma once

// Code hỗ trợ của protocol sinh ra (docs/formats/protocol.md, mục Mã sinh ra):
//
// - BoundedBytes, BoundedString, BoundedArray: có cỡ tối đa như schema, nằm ngay trong struct nên
//   tin nhắn không cấp phát (X.7). BoundedString luôn giữ UTF-8 hợp lệ.
// - Tick, ShortTick (ADR 0002) và ReplicatedId (ADR 0006): kiểu mạnh, không trộn được với số trần.
// - Encoder, Decoder: bọc bitstream với lỗi "dính": sau lỗi đầu tiên mọi lần ghi, đọc sau đó không
//   làm gì, và finish trả lỗi đó. Code sinh ra vì vậy là một dãy lệnh thẳng theo thứ tự trường,
//   không rẽ nhánh sau từng trường.
//
// Không đồng bộ; không cấp phát.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"
#include "engine/net/bitstream.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace orion::protocol {

// Bên gửi của một message (`from` trong schema).
enum class Sender : u8 {
    Client = 0,
    Server = 1,
};

// Số tick của mô phỏng, đếm từ 0 khi zone khởi động (ADR 0002).
struct Tick {
    u64 value = 0;

    friend constexpr auto operator<=>(Tick, Tick) noexcept = default;
};

// 16 bit thấp của một Tick; expand khôi phục tick đầy đủ từ một tick tham chiếu.
struct ShortTick {
    u16 value = 0;

    friend constexpr auto operator<=>(ShortTick, ShortTick) noexcept = default;
};

[[nodiscard]] constexpr ShortTick shorten(const Tick tick) noexcept {
    return ShortTick{static_cast<u16>(tick.value & 0xFFFFU)};
}

// Tick gần `reference` nhất có 16 bit thấp là `tick` (protocol.md, mục Kiểu). Cách đều hai phía
// (2^15) thì lấy phía trước; không bao giờ trả tick âm: gần 0 thì lấy phía sau.
[[nodiscard]] constexpr Tick expand(const ShortTick tick, const Tick reference) noexcept {
    const auto forward = static_cast<u16>(tick.value - static_cast<u16>(reference.value & 0xFFFFU));
    const u64 backward = u64{0x10000} - forward;
    if (forward > 0x8000U && reference.value >= backward) {
        return Tick{reference.value - backward};
    }
    return Tick{reference.value + forward};
}

// Id nhân bản do zone cấp (ADR 0006); 0 là không có thực thể.
struct ReplicatedId {
    u32 value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

    friend constexpr auto operator<=>(ReplicatedId, ReplicatedId) noexcept = default;
};

// Tới N byte.
template <usize N>
class BoundedBytes {
public:
    static_assert(N >= 1 && N <= 65'535);
    static constexpr usize kCapacity = N;

    // InvalidArgument khi dài hơn N; khi lỗi, nội dung cũ giữ nguyên.
    [[nodiscard]] Result<void> assign(const std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > N) {
            return fail(ErrorCode::InvalidArgument, "protocol: bytes dài hơn cỡ đã khai",
                        static_cast<i64>(bytes.size()));
        }
        std::ranges::copy(bytes, bytes_.begin());
        size_ = static_cast<u32>(bytes.size());
        return {};
    }

    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        return std::span(bytes_).first(size_);
    }
    [[nodiscard]] usize size() const noexcept { return size_; }

    // Cho Decoder: ghi thẳng vào bộ đệm rồi đặt cỡ.
    [[nodiscard]] std::span<std::byte, N> buffer() noexcept { return bytes_; }
    void resize(const usize size) noexcept {
        ORION_ASSERT(size <= N, "cỡ {} vượt {}", size, N);
        size_ = static_cast<u32>(size);
    }

    friend bool operator==(const BoundedBytes& a, const BoundedBytes& b) noexcept {
        return std::ranges::equal(a.view(), b.view());
    }

private:
    std::array<std::byte, N> bytes_{};
    u32 size_ = 0;
};

// Chuỗi UTF-8 hợp lệ tới N byte.
template <usize N>
class BoundedString {
public:
    static_assert(N >= 1 && N <= 65'535);
    static constexpr usize kCapacity = N;

    // InvalidArgument khi dài hơn N byte hay không phải UTF-8; khi lỗi, nội dung cũ giữ nguyên.
    [[nodiscard]] Result<void> assign(const std::string_view text) noexcept {
        if (text.size() > N || !core::is_valid_utf8(text)) {
            return fail(ErrorCode::InvalidArgument, "protocol: chuỗi quá dài hay không phải UTF-8",
                        static_cast<i64>(text.size()));
        }
        std::ranges::copy(text, chars_.begin());
        size_ = static_cast<u32>(text.size());
        return {};
    }

    [[nodiscard]] std::string_view view() const noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] usize size() const noexcept { return size_; }

    // Cho Decoder, sau khi bitstream đã kiểm UTF-8: ghi thẳng vào bộ đệm rồi đặt cỡ.
    [[nodiscard]] std::span<char, N> buffer() noexcept { return chars_; }
    void resize(const usize size) noexcept {
        ORION_ASSERT(size <= N, "cỡ {} vượt {}", size, N);
        size_ = static_cast<u32>(size);
    }

    friend bool operator==(const BoundedString& a, const BoundedString& b) noexcept {
        return a.view() == b.view();
    }

private:
    std::array<char, N> chars_{};
    u32 size_ = 0;
};

// Tới N phần tử kiểu T (T có hàm dựng mặc định).
template <class T, usize N>
class BoundedArray {
public:
    static_assert(N >= 1 && N <= 65'535);
    static constexpr usize kCapacity = N;

    // ResourceExhausted khi đã đủ N phần tử.
    [[nodiscard]] Result<void> push_back(const T& value) noexcept {
        if (size_ == N) {
            return fail(ErrorCode::ResourceExhausted, "protocol: mảng đã đủ cỡ đã khai");
        }
        items_[size_++] = value;
        return {};
    }

    // size <= N. Phần tử mới là T{}.
    void resize(const usize size) noexcept {
        ORION_ASSERT(size <= N, "cỡ {} vượt {}", size, N);
        for (usize i = size_; i < size; ++i) {
            items_[i] = T{};
        }
        size_ = static_cast<u32>(size);
    }
    void clear() noexcept { size_ = 0; }

    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::span<const T> view() const noexcept {
        return std::span(items_).first(size_);
    }
    [[nodiscard]] std::span<T> view() noexcept { return std::span(items_).first(size_); }
    [[nodiscard]] const T& operator[](const usize index) const noexcept {
        ORION_ASSERT(index < size_, "chỉ số {} ngoài {}", index, size_);
        return items_[index];
    }
    [[nodiscard]] T& operator[](const usize index) noexcept {
        ORION_ASSERT(index < size_, "chỉ số {} ngoài {}", index, size_);
        return items_[index];
    }
    [[nodiscard]] auto begin() const noexcept { return view().begin(); }
    [[nodiscard]] auto end() const noexcept { return view().end(); }

    friend bool operator==(const BoundedArray& a, const BoundedArray& b) noexcept {
        return std::ranges::equal(a.view(), b.view());
    }

private:
    std::array<T, N> items_{};
    u32 size_ = 0;
};

class Encoder {
public:
    explicit Encoder(std::span<std::byte> out) noexcept : writer_(out) {}

    void write_bool(bool value) noexcept;
    // InvalidArgument khi value cần hơn `count` bit.
    void write_bits(u64 value, u32 count) noexcept;
    // InvalidArgument khi value ngoài [min, max].
    template <std::integral T>
    void write_int(const T value, const i64 min, const i64 max) noexcept {
        if constexpr (std::unsigned_integral<T>) {
            if (value > static_cast<u64>(std::numeric_limits<i64>::max())) {
                reject();
                return;
            }
        }
        const auto wide = static_cast<i64>(value);
        if (wide < min || wide > max) {
            reject();
            return;
        }
        if (ok()) {
            writer_.write_ranged(wide, min, max);
        }
    }
    void write_quantized(f64 value, const net::Quantization& quantization) noexcept;
    template <usize N>
    void write_bytes(const BoundedBytes<N>& value) noexcept {
        if (ok()) {
            writer_.write_bytes(value.view(), N);
        }
    }
    template <usize N>
    void write_string(const BoundedString<N>& value) noexcept {
        if (ok()) {
            writer_.write_string(value.view(), N);
        }
    }
    // Độ dài của một mảng tới `max` phần tử.
    void write_length(usize length, usize max) noexcept;
    void write_tick(Tick tick) noexcept;
    void write_short_tick(ShortTick tick) noexcept;
    void write_replicated_id(ReplicatedId id) noexcept;
    // Giá trị không có trong schema (ví dụ enum lạ): InvalidArgument.
    void reject() noexcept;

    [[nodiscard]] bool ok() const noexcept { return !invalid_ && !writer_.overflowed(); }
    // Số byte đã ghi (đệm bit 0 tới hết byte); InvalidArgument khi có trường ngoài khoảng,
    // ResourceExhausted khi bộ đệm thiếu chỗ.
    [[nodiscard]] Result<usize> finish() const noexcept;

private:
    net::BitWriter writer_;
    bool invalid_ = false;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> in) noexcept : reader_(in) {}

    void read_bool(bool& out) noexcept;
    // `count` từ 1 tới số bit của T.
    template <std::unsigned_integral T>
    void read_bits(T& out, const u32 count) noexcept {
        ORION_ASSERT(count >= 1 && count <= std::numeric_limits<T>::digits, "{} bit", count);
        if (ok()) {
            keep(reader_.read_bits(count), out);
        }
    }
    // [min, max] vừa T (codegen chọn T theo khoảng).
    template <std::integral T>
    void read_int(T& out, const i64 min, const i64 max) noexcept {
        if (ok()) {
            keep(reader_.read_ranged(min, max), out);
        }
    }
    void read_quantized(f64& out, const net::Quantization& quantization) noexcept;
    template <usize N>
    void read_bytes(BoundedBytes<N>& out) noexcept {
        if (!ok()) {
            return;
        }
        const Result<usize> size = reader_.read_bytes(out.buffer(), N);
        if (!size) {
            error_ = size.error();
            return;
        }
        out.resize(*size);
    }
    template <usize N>
    void read_string(BoundedString<N>& out) noexcept {
        if (!ok()) {
            return;
        }
        const Result<usize> size = reader_.read_string(out.buffer(), N);
        if (!size) {
            error_ = size.error();
            return;
        }
        out.resize(*size);
    }
    // Độ dài của một mảng tới `max` phần tử; 0 khi đã có lỗi.
    [[nodiscard]] usize read_length(usize max) noexcept;
    void read_tick(Tick& out) noexcept;
    void read_short_tick(ShortTick& out) noexcept;
    void read_replicated_id(ReplicatedId& out) noexcept;
    // Giá trị đọc được nhưng không có trong schema (enum lạ, id lạ): InvalidArgument.
    void reject() noexcept;

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    // Lỗi đầu tiên; không có thì kiểm phần đệm và byte thừa như BitReader::finish.
    [[nodiscard]] Result<void> finish() const noexcept;

private:
    template <class From, std::integral T>
    void keep(const Result<From>& read, T& out) noexcept {
        if (!read) {
            error_ = read.error();
            return;
        }
        // BitReader đã kiểm khoảng, và codegen chọn T chứa được cả khoảng.
        out = static_cast<T>(*read);
    }

    net::BitReader reader_;
    std::optional<Error> error_;
};

}  // namespace orion::protocol
