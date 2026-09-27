#include "engine/math/random.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace orion::math {
namespace {

// Dãy tham chiếu của pcg32-demo trong pcg-c-basic (seed 42, stream 54), kiểm cả lúc biên dịch để
// chứng minh bộ sinh không phụ thuộc gì ngoài phép toán số nguyên.
constexpr std::array<u32, 6> kPcg32Reference{0xA15C'02B7U, 0x7B47'F409U, 0xBA1D'3330U,
                                             0x83D2'F293U, 0xBFA4'784BU, 0xCBED'606EU};

consteval bool pcg32_matches_reference() {
    Pcg32 rng(42, 54);
    for (const u32 expected : kPcg32Reference) {
        if (rng.next_u32() != expected) {
            return false;
        }
    }
    return true;
}
static_assert(pcg32_matches_reference());

// Dãy tham chiếu của splitmix64.c (Vigna) với seed 1234567.
constexpr std::array<u64, 5> kSplitMixReference{6457827717110365317ULL, 3203168211198807973ULL,
                                                9817491932198370423ULL, 4593380528125082431ULL,
                                                16408922859458223821ULL};

consteval bool splitmix_matches_reference() {
    SplitMix64 rng(1234567);
    for (const u64 expected : kSplitMixReference) {
        if (rng.next() != expected) {
            return false;
        }
    }
    return true;
}
static_assert(splitmix_matches_reference());

TEST(Random, Pcg32MatchesReferenceAtRunTime) {
    Pcg32 rng(42, 54);
    for (const u32 expected : kPcg32Reference) {
        EXPECT_EQ(rng.next_u32(), expected);
    }
    SplitMix64 mixer(1234567);
    for (const u64 expected : kSplitMixReference) {
        EXPECT_EQ(mixer.next(), expected);
    }
}

TEST(Random, CopiedGeneratorContinuesTheSameSequence) {
    Pcg32 rng(7, 3);
    static_cast<void>(rng.next_u64());
    Pcg32 snapshot = rng;
    EXPECT_EQ(snapshot, rng);
    for (u32 i = 0; i < 100; ++i) {
        EXPECT_EQ(snapshot.next_u32(), rng.next_u32());
    }
    SplitMix64 mixer(9);
    SplitMix64 mixer_copy = mixer;
    EXPECT_EQ(mixer_copy.next(), mixer.next());
    EXPECT_EQ(mixer_copy, mixer);
}

TEST(Random, StreamsAreDistinct) {
    Pcg32 a(1, 1);
    Pcg32 b(1, 2);
    u32 equal = 0;
    for (u32 i = 0; i < 1'000; ++i) {
        equal += a.next_u32() == b.next_u32() ? 1U : 0U;
    }
    EXPECT_LE(equal, 1U);
    EXPECT_NE(Pcg32(1, 1), Pcg32(2, 1));
}

TEST(Random, NextU64CombinesTwoDrawsHighFirst) {
    Pcg32 a(5, 6);
    Pcg32 b(5, 6);
    const u64 high = b.next_u32();
    const u64 low = b.next_u32();
    EXPECT_EQ(a.next_u64(), (high << 32U) | low);
}

TEST(Random, NextBelowStaysInRange) {
    Pcg32 rng(11, 0);
    constexpr std::array<u32, 8> kBounds{1U,    2U,           3U,           7U,
                                         1000U, 0x8000'0001U, 0xFFFF'FFFEU, 0xFFFF'FFFFU};
    for (const u32 bound : kBounds) {
        for (u32 i = 0; i < 2'000; ++i) {
            EXPECT_LT(rng.next_below(bound), bound);
        }
    }
    EXPECT_EQ(rng.next_below(1), 0U);
}

TEST(Random, NextBelowIsUniform) {
    // 600 000 lần tung với 6 mặt: kỳ vọng 100 000 mỗi mặt, độ lệch chuẩn khoảng 289. Seed cố định
    // nên test không chập chờn; ngưỡng 1% là hơn 3 độ lệch chuẩn.
    Pcg32 rng(2024, 1);
    std::array<u32, 6> counts{};
    for (u32 i = 0; i < 600'000; ++i) {
        ++counts.at(rng.next_below(6));
    }
    for (const u32 count : counts) {
        EXPECT_NEAR(count, 100'000, 1'000);
    }
}

TEST(Random, UnitFloatsStayInHalfOpenRange) {
    Pcg32 rng(99, 4);
    f64 sum32 = 0.0;
    f64 sum64 = 0.0;
    for (u32 i = 0; i < 100'000; ++i) {
        const f32 a = rng.next_f32();
        const f64 b = rng.next_f64();
        ASSERT_GE(a, 0.0F);
        ASSERT_LT(a, 1.0F);
        ASSERT_GE(b, 0.0);
        ASSERT_LT(b, 1.0);
        sum32 += a;
        sum64 += b;
    }
    EXPECT_NEAR(sum32 / 100'000.0, 0.5, 0.005);
    EXPECT_NEAR(sum64 / 100'000.0, 0.5, 0.005);
    // Giá trị lớn nhất có thể: (2^24 - 1) · 2^-24 < 1.
    EXPECT_LT(static_cast<f32>(0xFF'FFFFU) * 0x1.0p-24F, 1.0F);
    EXPECT_LT(static_cast<f64>(std::numeric_limits<u64>::max() >> 11U) * 0x1.0p-53, 1.0);
}

#if !ORION_SHIP
TEST(RandomDeathTest, NextBelowZeroIsAProgrammingError) {
    Pcg32 rng(1, 1);
    EXPECT_DEATH(static_cast<void>(rng.next_below(0)), "next_below");
}
#endif

}  // namespace
}  // namespace orion::math
