// Fuzz Address::parse (CLAUDE.md X.4): địa chỉ đọc từ cấu hình và dòng lệnh là dữ liệu từ ngoài.
// Mọi chuỗi cho ra một địa chỉ hoặc InvalidArgument. Địa chỉ được nhận thì dạng chữ chuẩn của nó
// đọc lại ra đúng nó, và dạng chuẩn không đổi khi in lại.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/net/address.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orion::net::Address;

    // Chép sang chuỗi thay vì ép kiểu con trỏ (X.3).
    std::string text(size, '\0');
    if (size > 0) {
        std::memcpy(text.data(), data, size);
    }
    const orion::Result<Address> address = Address::parse(text);
    if (!address.has_value()) {
        ORION_VERIFY(address.error().code() == orion::ErrorCode::InvalidArgument,
                     "mã lỗi khác InvalidArgument");
        return 0;
    }
    const std::string canonical = address->to_string();
    const orion::Result<Address> again = Address::parse(canonical);
    ORION_VERIFY(again.has_value() && *again == *address, "dạng chuẩn không đọc lại được: {}",
                 canonical);
    ORION_VERIFY(again->to_string() == canonical, "dạng chuẩn đổi khi in lại: {}", canonical);
    return 0;
}
