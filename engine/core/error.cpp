#include "engine/core/error.hpp"

#include <string_view>

namespace orion {

std::string_view to_string(const ErrorCode code) noexcept {
    switch (code) {
        using enum ErrorCode;
        case Cancelled:
            return "Cancelled";
        case InvalidArgument:
            return "InvalidArgument";
        case OutOfRange:
            return "OutOfRange";
        case NotFound:
            return "NotFound";
        case AlreadyExists:
            return "AlreadyExists";
        case PermissionDenied:
            return "PermissionDenied";
        case Unauthenticated:
            return "Unauthenticated";
        case ResourceExhausted:
            return "ResourceExhausted";
        case FailedPrecondition:
            return "FailedPrecondition";
        case Aborted:
            return "Aborted";
        case DeadlineExceeded:
            return "DeadlineExceeded";
        case Unavailable:
            return "Unavailable";
        case DataLoss:
            return "DataLoss";
        case Unimplemented:
            return "Unimplemented";
        case Internal:
            return "Internal";
        case Io:
            return "Io";
    }
    // Giá trị ngoài danh sách chỉ có thể đến từ dữ liệu ngoài; không assert (X.5).
    return "Unknown";
}

}  // namespace orion
