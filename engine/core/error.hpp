#pragma once

// Lỗi có thể xảy ra lúc chạy (CLAUDE.md X.5): file thiếu, gói hỏng, DB timeout. Hàm có thể lỗi trả
// orion::Result<T> (tức std::expected<T, Error>) và được đánh [[nodiscard]].
//
// Error mang một mã, một ngữ cảnh ngắn và một số chi tiết. Ngữ cảnh bắt buộc là chuỗi hằng lúc
// biên dịch, nên Error không bao giờ cấp phát và chỉ rộng 24 byte: trả Error trên hot path cũng rẻ.
// Chi tiết động (tên tệp, id người chơi) được log ở nơi xử lý lỗi, đúng một lần (X.5).

#include "engine/core/types.hpp"

#include <expected>
#include <format>
#include <string_view>

namespace orion {

// Giá trị tường minh và không bao giờ đổi, vì mã lỗi đi qua mạng và vào log (CLAUDE.md X.3).
enum class ErrorCode : u16 {
    Cancelled = 1,           // bên gọi đã huỷ thao tác
    InvalidArgument = 2,     // đầu vào sai định dạng, dù trạng thái hệ thống thế nào
    OutOfRange = 3,          // đầu vào đúng định dạng nhưng ngoài khoảng cho phép
    NotFound = 4,            // thứ được hỏi không tồn tại
    AlreadyExists = 5,       // thứ định tạo đã tồn tại
    PermissionDenied = 6,    // đã xác thực nhưng không có quyền
    Unauthenticated = 7,     // chưa xác thực, hoặc chứng thực sai, hết hạn
    ResourceExhausted = 8,   // hết chỗ, vượt hạn mức, vượt giới hạn tần suất
    FailedPrecondition = 9,  // trạng thái hiện tại không cho phép thao tác này
    Aborted = 10,            // xung đột đồng thời; bên gọi thử lại ở mức cao hơn
    DeadlineExceeded = 11,   // hết thời hạn trước khi xong
    Unavailable = 12,        // tạm thời không phục vụ được; thử lại sau được
    DataLoss = 13,           // dữ liệu hỏng: hash, checksum, chữ ký sai
    Unimplemented = 14,      // thao tác chưa hỗ trợ
    Internal = 15,           // bất biến nội bộ vỡ nhưng tiến trình vẫn an toàn để chạy tiếp
    Io = 16,                 // hệ điều hành báo lỗi khi đọc, ghi, mở tệp hay socket
};

// Tên mã lỗi dùng trong log và thông điệp, ví dụ "NotFound". Mã lạ (đọc từ ngoài) cho "Unknown".
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Ngữ cảnh của Error: chỉ dựng được từ chuỗi hằng lúc biên dịch, nên con trỏ luôn sống mãi.
class ErrorContext {
public:
    static constexpr usize kMaxLength = 96;

    // Không explicit: viết thẳng literal ở chỗ gọi, `fail(ErrorCode::NotFound, "pak: thiếu
    // entry")`.
    template <usize N>
    consteval ErrorContext(const char (&text)[N]) noexcept : text_(text), length_(N - 1) {
        static_assert(N - 1 <= kMaxLength, "ngữ cảnh lỗi phải ngắn (CLAUDE.md X.5)");
    }

    [[nodiscard]] constexpr const char* data() const noexcept { return text_; }
    [[nodiscard]] constexpr u32 size() const noexcept { return length_; }

private:
    const char* text_;
    u32 length_;
};

class [[nodiscard]] Error {
public:
    constexpr Error(const ErrorCode code, const ErrorContext context, const i64 detail = 0) noexcept
        : context_text_(context.data()),
          context_length_(context.size()),
          code_(code),
          detail_(detail) {}

    [[nodiscard]] constexpr ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] constexpr std::string_view context() const noexcept {
        return {context_text_, context_length_};
    }
    // Số đi kèm lỗi: errno, độ lệch trong tệp, giá trị vượt khoảng. 0 khi không có.
    [[nodiscard]] constexpr i64 detail() const noexcept { return detail_; }

    friend constexpr bool operator==(const Error& a, const Error& b) noexcept {
        return a.code_ == b.code_ && a.detail_ == b.detail_ && a.context() == b.context();
    }

private:
    // Thứ tự trường giữ Error ở 24 byte trên kiến trúc 64 bit.
    const char* context_text_;
    u32 context_length_;
    ErrorCode code_;
    i64 detail_;
};

template <class T>
using Result = std::expected<T, Error>;

// Viết gọn cho `return std::unexpected(Error{...})`.
[[nodiscard]] constexpr std::unexpected<Error> fail(const ErrorCode code,
                                                    const ErrorContext context,
                                                    const i64 detail = 0) noexcept {
    return std::unexpected<Error>(Error{code, context, detail});
}

}  // namespace orion

// "NotFound: pak không có entry (chi tiết 42)"; phần chi tiết bỏ đi khi bằng 0.
template <>
struct std::formatter<orion::Error> : std::formatter<std::string_view> {
    template <class FormatContext>
    auto format(const orion::Error& error, FormatContext& ctx) const {
        auto out =
            std::format_to(ctx.out(), "{}: {}", orion::to_string(error.code()), error.context());
        if (error.detail() != 0) {
            out = std::format_to(out, " (chi tiết {})", error.detail());
        }
        return out;
    }
};

template <>
struct std::formatter<orion::ErrorCode> : std::formatter<std::string_view> {
    template <class FormatContext>
    auto format(const orion::ErrorCode code, FormatContext& ctx) const {
        return std::formatter<std::string_view>::format(orion::to_string(code), ctx);
    }
};
