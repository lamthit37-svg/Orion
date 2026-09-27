#include "engine/core/utf8.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace orion::core {
namespace {

// Bộ giải mã tham chiếu, viết thẳng theo RFC 3629 bằng cách tính điểm mã, để đối chiếu với bản
// dùng bảng khoảng của is_valid_utf8.
bool reference_valid(const std::string_view text) {
    usize i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<u8>(text[i]);
        usize length = 0;
        u32 code = 0;
        if (lead < 0x80U) {
            length = 1;
            code = lead;
        } else if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code = lead & 0x07U;
        } else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (usize k = 1; k < length; ++k) {
            const auto next = static_cast<u8>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        constexpr std::array<u32, 5> kMinimum{0, 0, 0x80, 0x800, 0x10000};
        if (code < kMinimum[length] || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        i += length;
    }
    return true;
}

TEST(Utf8, AcceptsWellFormedText) {
    EXPECT_TRUE(is_valid_utf8(""));
    EXPECT_TRUE(is_valid_utf8("hello"));
    EXPECT_TRUE(is_valid_utf8("Tiếng Việt có dấu: ắ ằ ẳ ẵ ặ Đ đ"));
    EXPECT_TRUE(is_valid_utf8("中文 한국어 日本語"));
    EXPECT_TRUE(is_valid_utf8("\xF0\x9F\x98\x80"));                  // U+1F600
    EXPECT_TRUE(is_valid_utf8("\x7F\xC2\x80\xDF\xBF"));              // U+007F, U+0080, U+07FF
    EXPECT_TRUE(is_valid_utf8("\xE0\xA0\x80\xEF\xBF\xBF"));          // U+0800, U+FFFF
    EXPECT_TRUE(is_valid_utf8("\xF0\x90\x80\x80\xF4\x8F\xBF\xBF"));  // U+10000, U+10FFFF
}

TEST(Utf8, RejectsMalformedText) {
    EXPECT_FALSE(is_valid_utf8("\xC0\x80"));          // mã hoá thừa của U+0000
    EXPECT_FALSE(is_valid_utf8("\xE0\x80\x80"));      // mã hoá thừa
    EXPECT_FALSE(is_valid_utf8("\xF0\x80\x80\x80"));  // mã hoá thừa
    EXPECT_FALSE(is_valid_utf8("\xED\xA0\x80"));      // surrogate U+D800
    EXPECT_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // U+110000
    EXPECT_FALSE(is_valid_utf8("\x80"));              // byte nối đứng một mình
    EXPECT_FALSE(is_valid_utf8("\xE2\x82"));          // bị cắt giữa chừng
    EXPECT_FALSE(is_valid_utf8("\xFE"));
    EXPECT_FALSE(is_valid_utf8("\xFF"));
    EXPECT_FALSE(is_valid_utf8("ok\xC3("));  // byte thứ hai không phải byte nối
}

TEST(Utf8, MatchesReferenceOnGeneratedBytes) {
    // xorshift64 với seed cố định (X.4): chạy lại cho đúng cùng chuỗi byte.
    constexpr u64 kSeed = 0x9E37'79B9'7F4A'7C15ULL;
    u64 state = kSeed;
    std::string text;
    for (int round = 0; round < 20'000; ++round) {
        text.clear();
        const usize length = state % 8U;
        for (usize i = 0; i < length; ++i) {
            state ^= state << 13U;
            state ^= state >> 7U;
            state ^= state << 17U;
            // Nghiêng về các byte có nghĩa trong UTF-8 để chạm nhiều nhánh.
            const auto raw = static_cast<u8>(state >> 56U);
            text.push_back(static_cast<char>((state & 1U) != 0U ? raw : (raw | 0x80U)));
        }
        state ^= state << 13U;
        ASSERT_EQ(is_valid_utf8(text), reference_valid(text))
            << "seed " << kSeed << ", vòng " << round;
    }
}

TEST(Utf8, SequenceLengthFindsEachCodePoint) {
    const std::string_view text = "aĐ中\xF0\x9F\x98\x80\xFF";
    EXPECT_EQ(utf8_sequence_length(text, 0), 1U);
    EXPECT_EQ(utf8_sequence_length(text, 1), 2U);
    EXPECT_EQ(utf8_sequence_length(text, 3), 3U);
    EXPECT_EQ(utf8_sequence_length(text, 6), 4U);
    EXPECT_EQ(utf8_sequence_length(text, 10), 0U) << "0xFF không mở đầu điểm mã nào";
    EXPECT_EQ(utf8_sequence_length(text, 2), 0U) << "byte nối không mở đầu điểm mã";
    EXPECT_EQ(utf8_sequence_length("\xE4\xB8", 0), 0U) << "bị cắt giữa chừng";
}

TEST(Utf8, DropIncompleteTailKeepsWholeCodePoints) {
    EXPECT_EQ(utf8_drop_incomplete_tail(""), "");
    EXPECT_EQ(utf8_drop_incomplete_tail("abc"), "abc");
    EXPECT_EQ(utf8_drop_incomplete_tail("a\xE4\xB8\xAD"), "a\xE4\xB8\xAD");
    EXPECT_EQ(utf8_drop_incomplete_tail("a\xE4\xB8"), "a");
    EXPECT_EQ(utf8_drop_incomplete_tail("a\xF0\x9F\x98"), "a");
    EXPECT_EQ(utf8_drop_incomplete_tail("a\xC3"), "a");
}

TEST(Utf8, LengthCountsCodePoints) {
    EXPECT_EQ(utf8_length(""), 0U);
    EXPECT_EQ(utf8_length("abc"), 3U);
    EXPECT_EQ(utf8_length("Việt"), 4U);
    EXPECT_EQ(utf8_length("\xF0\x9F\x98\x80"), 1U);
}

TEST(Utf8, TruncateNeverSplitsACodePoint) {
    const std::string_view text = "aĐ中\xF0\x9F\x98\x80";  // 1 + 2 + 3 + 4 byte
    EXPECT_EQ(utf8_truncate(text, 100), text);
    EXPECT_EQ(utf8_truncate(text, 1), "a");
    EXPECT_EQ(utf8_truncate(text, 2), "a");
    EXPECT_EQ(utf8_truncate(text, 3), "aĐ");
    EXPECT_EQ(utf8_truncate(text, 5), "aĐ");
    EXPECT_EQ(utf8_truncate(text, 6), "aĐ中");
    EXPECT_EQ(utf8_truncate(text, 9), "aĐ中");
    EXPECT_EQ(utf8_truncate(text, 0), "");
    for (usize limit = 0; limit <= text.size(); ++limit) {
        EXPECT_TRUE(is_valid_utf8(utf8_truncate(text, limit))) << "giới hạn " << limit;
    }
}

}  // namespace
}  // namespace orion::core
