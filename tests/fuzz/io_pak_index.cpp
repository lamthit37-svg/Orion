// Fuzz PakIndex::parse_image (CLAUDE.md X.4, X.9; docs/formats/pak.md): pak tải từ CDN và nằm trên
// đĩa người chơi là dữ liệu từ ngoài. Mọi dãy byte cho ra một index hợp lệ hoặc một lỗi trong bảng
// "Lỗi" của định dạng, không bao giờ UB. Index được nhận thì mọi entry nằm trong vùng dữ liệu, có
// đường dẫn ảo hợp lệ, và tìm lại được đúng chỉ số của nó.

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/path.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orion::ErrorCode;
    using orion::io::PakIndex;

    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> image(size);
    if (size > 0) {
        std::memcpy(image.data(), data, size);
    }
    const orion::Result<PakIndex> index = PakIndex::parse_image(image);
    if (!index.has_value()) {
        const ErrorCode code = index.error().code();
        ORION_VERIFY(code == ErrorCode::InvalidArgument || code == ErrorCode::Unimplemented ||
                         code == ErrorCode::DataLoss,
                     "mã lỗi ngoài bảng của định dạng");
        return 0;
    }
    for (orion::usize i = 0; i < index->entry_count(); ++i) {
        const orion::io::PakEntry& entry = index->entry(i);
        ORION_VERIFY(entry.offset >= orion::io::kPakHeaderSize && entry.offset <= size &&
                         entry.stored_size <= size - entry.offset,
                     "blob nằm ngoài tệp");
        ORION_VERIFY(orion::io::is_valid_virtual_path(index->path(i)), "đường dẫn sai luật");
        ORION_VERIFY(index->find(index->path(i)) == std::optional<orion::usize>(i),
                     "không tìm lại được entry");
    }
    for (orion::usize k = 1; k <= index->dictionary_count(); ++k) {
        ORION_VERIFY(!index->dictionary(k).empty(), "dictionary rỗng");
    }
    return 0;
}
