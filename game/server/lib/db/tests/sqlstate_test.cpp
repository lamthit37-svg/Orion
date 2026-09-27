// SQLSTATE thành Error (detail/sqlstate.hpp) và mã gói SQLSTATE (db.hpp). Mã lấy từ phụ lục
// "PostgreSQL Error Codes" của tài liệu PostgreSQL.

#include "game/server/lib/db/detail/sqlstate.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/db.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace orion::db {
namespace {

TEST(Sqlstate, CodesPackFiveCharactersAndUnpackAgain) {
    EXPECT_EQ(sqlstate_code("23505"), (i64{'2'} << 32U) | (i64{'3'} << 24U) | (i64{'5'} << 16U) |
                                          (i64{'0'} << 8U) | i64{'5'});
    for (const std::string_view state : {"23505", "40P01", "XX000", "P0001", "0A000"}) {
        const std::array<char, 5> text = sqlstate_text(sqlstate_code(state));
        EXPECT_EQ(std::string_view(text.data(), text.size()), state);
    }
    EXPECT_NE(kUniqueViolation, kForeignKeyViolation);
    EXPECT_NE(kSerializationFailure, kDeadlockDetected);
}

struct Case {
    std::string_view state;
    ErrorCode code;
};

TEST(Sqlstate, SpecificCodesWinOverTheirClass) {
    constexpr std::array kCases{
        Case{"23505", ErrorCode::AlreadyExists},
        Case{"23503", ErrorCode::FailedPrecondition},
        Case{"23514", ErrorCode::FailedPrecondition},
        Case{"40001", ErrorCode::Aborted},
        Case{"40P01", ErrorCode::Aborted},
        Case{"40002", ErrorCode::Aborted},
        Case{"25P02", ErrorCode::Aborted},
        Case{"25006", ErrorCode::FailedPrecondition},
        Case{"55P03", ErrorCode::Aborted},
        Case{"55006", ErrorCode::FailedPrecondition},
        Case{"57014", ErrorCode::DeadlineExceeded},
        Case{"57P01", ErrorCode::Unavailable},
        Case{"22001", ErrorCode::OutOfRange},
        Case{"22003", ErrorCode::OutOfRange},
        Case{"22008", ErrorCode::OutOfRange},
        Case{"22P02", ErrorCode::InvalidArgument},
        Case{"22021", ErrorCode::InvalidArgument},
        Case{"42501", ErrorCode::PermissionDenied},
        Case{"42601", ErrorCode::Internal},
        Case{"42P01", ErrorCode::Internal},
        Case{"XX001", ErrorCode::DataLoss},
        Case{"XX002", ErrorCode::DataLoss},
        Case{"XX000", ErrorCode::Internal},
        Case{"08006", ErrorCode::Unavailable},
        Case{"0A000", ErrorCode::Unimplemented},
        Case{"28P01", ErrorCode::Unauthenticated},
        Case{"53300", ErrorCode::ResourceExhausted},
        Case{"54001", ErrorCode::ResourceExhausted},
        Case{"58030", ErrorCode::Unavailable},
        Case{"P0001", ErrorCode::FailedPrecondition},
    };
    for (const Case& test : kCases) {
        const Error error = detail::sqlstate_error(test.state);
        EXPECT_EQ(error.code(), test.code) << test.state;
        EXPECT_EQ(error.detail(), sqlstate_code(test.state)) << test.state;
        EXPECT_FALSE(error.context().empty()) << test.state;
    }
}

// SQLSTATE đến từ server: lớp lạ hay chuỗi hỏng là Internal, không bao giờ là assert (X.5).
TEST(Sqlstate, UnknownOrMalformedStatesAreInternal) {
    for (const std::string_view state : {"ZZ999", "3D000", "", "2350", "235050", "\xFF\xFE"}) {
        const Error error = detail::sqlstate_error(state);
        EXPECT_EQ(error.code(), ErrorCode::Internal) << state;
        EXPECT_EQ(error.detail(), sqlstate_code(state)) << state;
    }
}

}  // namespace
}  // namespace orion::db
