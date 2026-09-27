#include "game/server/lib/db/detail/sqlstate.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/db/db.hpp"

#include <array>
#include <string_view>

namespace orion::db {

std::array<char, 5> sqlstate_text(const i64 code) noexcept {
    std::array<char, 5> text{};
    const auto bits = static_cast<u64>(code);
    for (usize i = 0; i < text.size(); ++i) {
        text[text.size() - 1 - i] = static_cast<char>((bits >> (8U * i)) & 0xFFU);
    }
    return text;
}

namespace detail {
namespace {

struct Mapping {
    std::string_view state;  // năm ký tự: một mã; hai ký tự: cả lớp
    ErrorCode code;
    ErrorContext context;
};

// Mã cụ thể đứng trước lớp của nó; dòng đầu tiên khớp thắng. Aborted là "chạy lại cả transaction
// thì có thể qua"; Internal là lỗi của chính câu SQL hay schema (thiếu migration), sửa code mới
// qua.
constexpr std::array kMappings{
    Mapping{"23505", ErrorCode::AlreadyExists, "db: trùng giá trị của ràng buộc duy nhất"},
    Mapping{"40001", ErrorCode::Aborted, "db: xung đột serializable, chạy lại transaction"},
    Mapping{"40P01", ErrorCode::Aborted, "db: deadlock, chạy lại transaction"},
    Mapping{"25P02", ErrorCode::Aborted, "db: transaction đã hỏng vì một câu trước đó lỗi"},
    Mapping{"55P03", ErrorCode::Aborted, "db: không lấy được khoá trong lock_timeout"},
    Mapping{"57014", ErrorCode::DeadlineExceeded, "db: server huỷ câu vì statement_timeout"},
    Mapping{"22001", ErrorCode::OutOfRange, "db: chuỗi dài hơn cột"},
    Mapping{"22003", ErrorCode::OutOfRange, "db: số ngoài khoảng của kiểu"},
    Mapping{"22008", ErrorCode::OutOfRange, "db: thời điểm ngoài khoảng của kiểu"},
    Mapping{"42501", ErrorCode::PermissionDenied, "db: vai trò DB không đủ quyền"},
    Mapping{"XX001", ErrorCode::DataLoss, "db: dữ liệu trên server bị hỏng"},
    Mapping{"XX002", ErrorCode::DataLoss, "db: index trên server bị hỏng"},
    Mapping{"08", ErrorCode::Unavailable, "db: lỗi kết nối với server"},
    Mapping{"0A", ErrorCode::Unimplemented, "db: server không hỗ trợ tính năng này"},
    Mapping{"22", ErrorCode::InvalidArgument, "db: dữ liệu không hợp lệ với kiểu hay hàm"},
    Mapping{"23", ErrorCode::FailedPrecondition, "db: vi phạm ràng buộc toàn vẹn"},
    Mapping{"25", ErrorCode::FailedPrecondition, "db: trạng thái transaction không cho phép câu"},
    Mapping{"28", ErrorCode::Unauthenticated, "db: xác thực với server thất bại"},
    Mapping{"40", ErrorCode::Aborted, "db: server huỷ transaction, chạy lại"},
    Mapping{"42", ErrorCode::Internal, "db: câu SQL hay schema sai"},
    Mapping{"53", ErrorCode::ResourceExhausted, "db: server hết tài nguyên"},
    Mapping{"54", ErrorCode::ResourceExhausted, "db: vượt giới hạn của server"},
    Mapping{"55", ErrorCode::FailedPrecondition, "db: đối tượng chưa ở trạng thái cần"},
    Mapping{"57", ErrorCode::Unavailable, "db: server đang dừng hay khởi động lại"},
    Mapping{"58", ErrorCode::Unavailable, "db: server gặp lỗi hệ thống"},
    Mapping{"P0", ErrorCode::FailedPrecondition, "db: hàm PL/pgSQL báo lỗi"},
    Mapping{"XX", ErrorCode::Internal, "db: lỗi nội bộ của server"},
};

}  // namespace

Error sqlstate_error(const std::string_view state) noexcept {
    const i64 code = sqlstate_code(state);
    if (state.size() != 5) {
        return Error{ErrorCode::Internal, "db: SQLSTATE không đúng năm ký tự", code};
    }
    for (const Mapping& mapping : kMappings) {
        if (state.starts_with(mapping.state)) {
            return Error{mapping.code, mapping.context, code};
        }
    }
    return Error{ErrorCode::Internal, "db: SQLSTATE không có trong bảng", code};
}

}  // namespace detail
}  // namespace orion::db
