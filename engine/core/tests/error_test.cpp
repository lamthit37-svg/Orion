#include "engine/core/error.hpp"

#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <format>
#include <utility>

namespace orion {
namespace {

static_assert(sizeof(Error) == 24, "Error phải rẻ để trả trên hot path (X.5)");

Result<i32> parse_positive(const i32 value) {
    if (value <= 0) {
        return fail(ErrorCode::OutOfRange, "giá trị phải dương", value);
    }
    return value;
}

TEST(Error, CarriesCodeContextAndDetail) {
    const Error error{ErrorCode::NotFound, "pak: không có entry", 42};
    EXPECT_EQ(error.code(), ErrorCode::NotFound);
    EXPECT_EQ(error.context(), "pak: không có entry");
    EXPECT_EQ(error.detail(), 42);
    EXPECT_EQ(error, (Error{ErrorCode::NotFound, "pak: không có entry", 42}));
    EXPECT_NE(error, (Error{ErrorCode::NotFound, "pak: không có entry", 43}));
    EXPECT_NE(error, (Error{ErrorCode::DataLoss, "pak: không có entry", 42}));
}

TEST(Error, ResultPropagatesValueOrError) {
    const auto good = parse_positive(5);
    ASSERT_TRUE(good.has_value());
    EXPECT_EQ(*good, 5);

    const auto bad = parse_positive(-3);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(bad.error().detail(), -3);
}

TEST(Error, FormatsForLogs) {
    EXPECT_EQ(std::format("{}", Error{ErrorCode::DataLoss, "pak: hash sai"}),
              "DataLoss: pak: hash sai");
    EXPECT_EQ(std::format("{}", Error{ErrorCode::Io, "mở tệp", 2}), "Io: mở tệp (chi tiết 2)");
    EXPECT_EQ(std::format("{}", ErrorCode::Unavailable), "Unavailable");
}

TEST(Error, EveryCodeHasANameAndUnknownValuesDoNotAssert) {
    for (u16 raw = 1; raw <= 16; ++raw) {
        EXPECT_NE(to_string(static_cast<ErrorCode>(raw)), "Unknown") << "mã " << raw;
    }
    EXPECT_EQ(to_string(static_cast<ErrorCode>(0)), "Unknown");
    EXPECT_EQ(to_string(static_cast<ErrorCode>(999)), "Unknown");
}

TEST(Error, CodesKeepTheirWireValues) {
    // Mã lỗi đi qua mạng và vào log: đổi số của một mã cũ là phá protocol (CLAUDE.md X.3).
    EXPECT_EQ(std::to_underlying(ErrorCode::Cancelled), 1);
    EXPECT_EQ(std::to_underlying(ErrorCode::InvalidArgument), 2);
    EXPECT_EQ(std::to_underlying(ErrorCode::ResourceExhausted), 8);
    EXPECT_EQ(std::to_underlying(ErrorCode::DataLoss), 13);
    EXPECT_EQ(std::to_underlying(ErrorCode::Io), 16);
}

}  // namespace
}  // namespace orion
