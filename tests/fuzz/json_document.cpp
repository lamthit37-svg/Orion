// Fuzz Document::parse của game/server/lib/http (CLAUDE.md X.4): body JSON là dữ liệu từ ngoài
// (X.5). Mọi input cho ra một tài liệu, hay một lỗi thuộc ba mã json.hpp đã hứa, không bao giờ UB.
// Với tài liệu hợp lệ: mọi chuỗi và khoá là UTF-8 hợp lệ, số là hữu hạn, cây không sâu hơn
// kMaxDepth; và Writer ghi lại nó thành một chuỗi mà parse rồi ghi lần nữa ra đúng chuỗi đó, tức
// Writer luôn ra JSON đọc lại được và không làm mất giá trị nào.

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"
#include "game/server/lib/http/json.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using orion::ErrorCode;
using orion::Result;
namespace json = orion::http::json;

void check_text(const std::string_view text) {
    ORION_VERIFY(orion::core::is_valid_utf8(text), "json: chuỗi đọc được không phải UTF-8");
}

// Một array hay object đang được ghi.
struct Frame {
    bool object = false;
    json::Object::Iterator member;
    json::Object::Iterator members_end;
    json::Array::Iterator element;
    json::Array::Iterator elements_end;
};

// Ghi một giá trị; array và object được mở và đẩy lên `stack` để vòng lặp ghi con của chúng.
void emit(json::Writer& writer, std::vector<Frame>& stack, const json::Value value) {
    switch (value.kind()) {
        case json::Kind::Null:
            writer.null();
            return;
        case json::Kind::Boolean: {
            const Result<bool> truth = value.boolean();
            ORION_VERIFY(truth.has_value(), "json: Boolean không đọc được");
            writer.boolean(*truth);
            return;
        }
        case json::Kind::Number: {
            const Result<orion::f64> real = value.real();
            ORION_VERIFY(real.has_value() && std::isfinite(*real), "json: số không hữu hạn");
            const Result<orion::i64> integer = value.integer();
            if (integer) {
                writer.integer(*integer);
            } else {
                writer.real(*real);
            }
            return;
        }
        case json::Kind::String: {
            const Result<std::string_view> text = value.string();
            ORION_VERIFY(text.has_value(), "json: String không đọc được");
            check_text(*text);
            writer.string(*text);
            return;
        }
        case json::Kind::Array: {
            const Result<json::Array> array = value.array();
            ORION_VERIFY(array.has_value(), "json: Array không đọc được");
            writer.begin_array();
            Frame& frame = stack.emplace_back();
            frame.element = array->begin();
            frame.elements_end = array->end();
            return;
        }
        case json::Kind::Object: {
            const Result<json::Object> object = value.object();
            ORION_VERIFY(object.has_value(), "json: Object không đọc được");
            writer.begin_object();
            Frame& frame = stack.emplace_back();
            frame.object = true;
            frame.member = object->begin();
            frame.members_end = object->end();
            return;
        }
    }
}

// Ghi cả cây bằng vòng lặp (không đệ quy, như code của game/).
std::string serialize(const json::Value root) {
    json::Writer writer;
    std::vector<Frame> stack;
    emit(writer, stack, root);
    while (!stack.empty()) {
        ORION_VERIFY(stack.size() <= json::kMaxDepth, "json: cây sâu hơn kMaxDepth");
        Frame& top = stack.back();
        if (top.object && top.member != top.members_end) {
            const json::Member member = *top.member++;
            check_text(member.key);
            writer.key(member.key);
            emit(writer, stack, member.value);
        } else if (!top.object && top.element != top.elements_end) {
            const json::Value element = *top.element++;
            emit(writer, stack, element);
        } else {
            if (top.object) {
                writer.end_object();
            } else {
                writer.end_array();
            }
            stack.pop_back();
        }
    }
    return writer.finish();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text = orion::core::as_chars(std::as_bytes(std::span(data, size)));
    json::Document document;
    const Result<json::Value> root = document.parse(text);
    if (!root) {
        const ErrorCode code = root.error().code();
        ORION_VERIFY(code == ErrorCode::InvalidArgument || code == ErrorCode::OutOfRange ||
                         code == ErrorCode::ResourceExhausted,
                     "mã lỗi ngoài hợp đồng của json.hpp: {}", code);
        return 0;
    }
    const std::string written = serialize(*root);
    json::Document again;
    const Result<json::Value> reparsed = again.parse(written);
    ORION_VERIFY(reparsed.has_value(), "json: Writer ghi ra JSON không đọc lại được");
    ORION_VERIFY(serialize(*reparsed) == written, "json: ghi, đọc, ghi lại ra chuỗi khác");
    return 0;
}
