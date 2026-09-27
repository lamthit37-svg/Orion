#pragma once

// Bộ sinh số giả ngẫu nhiên tất định (CLAUDE.md X.11): cùng seed cho cùng dãy trên mọi toolchain.
// <random> bị cấm trong game/shared vì các distribution của nó cho kết quả khác nhau giữa thư viện
// chuẩn; ở đây mọi phép chỉ dùng số nguyên và phép nhân số thực với lũy thừa của 2.

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"

namespace orion::math {

// SplitMix64 (Steele, Lea, Flood 2014; bản tham chiếu của Vigna). Dùng để trộn và dẫn xuất seed,
// ví dụ seed của từng thực thể từ seed của zone, không dùng làm nguồn ngẫu nhiên cho mô phỏng.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(const u64 seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr u64 next() noexcept {
        state_ += 0x9E37'79B9'7F4A'7C15ULL;
        u64 z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] friend constexpr bool operator==(const SplitMix64&,
                                                   const SplitMix64&) noexcept = default;

private:
    u64 state_;
};

// PCG32, bản XSH-RR 64/32 của O'Neill (pcg-random.org, pcg_basic.c): trạng thái 16 byte, chép được
// để lưu vào snapshot và replay. stream chọn một trong 2^63 dãy độc lập với cùng seed.
class Pcg32 {
public:
    constexpr Pcg32(const u64 seed, const u64 stream) noexcept : increment_((stream << 1U) | 1U) {
        static_cast<void>(next_u32());
        state_ += seed;
        static_cast<void>(next_u32());
    }

    [[nodiscard]] constexpr u32 next_u32() noexcept {
        const u64 old = state_;
        state_ = (old * 6'364'136'223'846'793'005ULL) + increment_;
        const auto xorshifted = static_cast<u32>(((old >> 18U) ^ old) >> 27U);
        const auto rotation = static_cast<u32>(old >> 59U);
        return (xorshifted >> rotation) | (xorshifted << ((0U - rotation) & 31U));
    }

    // Hai lần next_u32 theo thứ tự cố định: phần cao trước.
    [[nodiscard]] constexpr u64 next_u64() noexcept {
        const u64 high = next_u32();
        const u64 low = next_u32();
        return (high << 32U) | low;
    }

    // Số nguyên đều trong [0, bound), không lệch (Lemire 2019). Tiền điều kiện: bound > 0.
    [[nodiscard]] constexpr u32 next_below(const u32 bound) noexcept {
        ORION_ASSERT(bound > 0, "next_below cần bound > 0");
        u64 product = u64{next_u32()} * bound;
        auto low = static_cast<u32>(product);
        if (low < bound) {
            // Số giá trị thừa của 2^32 khi chia cho bound; loại chúng để mọi kết quả đồng xác suất.
            const u32 threshold = (0U - bound) % bound;
            while (low < threshold) {
                product = u64{next_u32()} * bound;
                low = static_cast<u32>(product);
            }
        }
        return static_cast<u32>(product >> 32U);
    }

    // Số thực đều trong [0, 1) với 24 bit ngẫu nhiên, đúng số bit ý nghĩa của f32.
    [[nodiscard]] constexpr f32 next_f32() noexcept {
        return static_cast<f32>(next_u32() >> 8U) * 0x1.0p-24F;
    }

    // Số thực đều trong [0, 1) với 53 bit ngẫu nhiên.
    [[nodiscard]] constexpr f64 next_f64() noexcept {
        return static_cast<f64>(next_u64() >> 11U) * 0x1.0p-53;
    }

    [[nodiscard]] friend constexpr bool operator==(const Pcg32&, const Pcg32&) noexcept = default;

private:
    u64 state_ = 0;
    u64 increment_;
};

}  // namespace orion::math
