#include "game/server/lib/http/json.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/core/utf8.hpp"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::http::json {
namespace {

// Chỉ số "không có" trong danh sách con.
constexpr u32 kNone = std::numeric_limits<u32>::max();
// Sức chứa ban đầu của parser; simdjson tự nới khi tài liệu lớn hơn, tới kMaxDocumentBytes.
constexpr usize kInitialCapacity = 4'096;

[[nodiscard]] std::unexpected<Error> wrong_kind(const Kind kind) noexcept {
    return fail(ErrorCode::InvalidArgument, "json: giá trị sai kiểu", std::to_underlying(kind));
}

[[nodiscard]] std::unexpected<Error> parse_error(const simdjson::error_code code) noexcept {
    const auto detail = static_cast<i64>(code);
    switch (code) {
        case simdjson::DEPTH_ERROR:
            return fail(ErrorCode::ResourceExhausted, "json: lồng sâu hơn 32 cấp", detail);
        case simdjson::CAPACITY:
        case simdjson::MEMALLOC:
            return fail(ErrorCode::ResourceExhausted, "json: tài liệu vượt sức chứa của parser",
                        detail);
        case simdjson::BIGINT_ERROR:
            return fail(ErrorCode::OutOfRange, "json: số nguyên không vừa 64 bit", detail);
        case simdjson::UTF8_ERROR:
            return fail(ErrorCode::InvalidArgument, "json: tài liệu không phải UTF-8 hợp lệ",
                        detail);
        default:
            return fail(ErrorCode::InvalidArgument, "json: tài liệu sai cú pháp", detail);
    }
}

}  // namespace

namespace detail {

// Một giá trị trong cây phẳng. Con của array và object nối thành danh sách bằng first và next, theo
// thứ tự trong tài liệu.
struct Node {
    std::string_view key;   // tên, khi là thành viên của một object
    std::string_view text;  // String
    i64 integer = 0;        // Number vừa i64
    f64 real = 0.0;         // Number
    u32 first = kNone;      // Array, Object: con đầu tiên
    u32 next = kNone;       // con kế tiếp của cùng cha
    u32 size = 0;           // Array, Object: số con
    Kind kind = Kind::Null;
    bool truth = false;     // Boolean
    bool integral = false;  // Number viết như số nguyên và vừa i64
    bool big = false;       // Number viết như số nguyên mà không vừa i64
};

// Bộ nhớ của một Document: parser của simdjson (chuỗi của cây trỏ vào bộ đệm của nó), cây đã chép,
// và chỗ tạm của lần kiểm khoá trùng (giữ lại giữa các lần parse để khỏi cấp phát lại).
class Tree {
public:
    class Builder;

    simdjson::dom::parser parser{kMaxDocumentBytes};
    std::vector<Node> nodes;
    std::vector<std::string_view> keys;
};

}  // namespace detail

namespace {

[[nodiscard]] const detail::Node& node_at(const detail::Tree* tree, const u32 index) noexcept {
    return tree->nodes[index];
}

}  // namespace

// Chép cây của simdjson sang Tree::nodes bằng vòng lặp trên ngăn xếp cỡ cố định: không đệ quy
// (CLAUDE.md X.3).
class detail::Tree::Builder {
public:
    explicit Builder(Tree& tree) noexcept : tree_(&tree) {}

    [[nodiscard]] Result<void> build(simdjson::dom::element root) noexcept;

private:
    // Một array hay object đang được chép; iterator là của simdjson.
    struct Frame {
        u32 node = kNone;
        u32 last = kNone;
        bool object = false;
        simdjson::dom::array::iterator element;
        simdjson::dom::array::iterator elements_end;
        simdjson::dom::object::iterator member;
        simdjson::dom::object::iterator members_end;
    };

    [[nodiscard]] Result<void> add(simdjson::dom::element element, std::string_view key) noexcept;
    static void fill_scalar(Node& node, simdjson::dom::element element) noexcept;
    [[nodiscard]] Result<void> check_unique_keys(u32 object) noexcept;

    Tree* tree_;
    std::array<Frame, kMaxDepth + 1> stack_{};
    usize depth_ = 0;
};

void detail::Tree::Builder::fill_scalar(Node& node, const simdjson::dom::element element) noexcept {
    // get(...) trên đúng kiểu mà type() vừa báo không lỗi được; giá trị mặc định của Node giữ
    // nguyên nếu có.
    switch (element.type()) {
        case simdjson::dom::element_type::INT64:
            node.kind = Kind::Number;
            node.integral = element.get_int64().get(node.integer) == simdjson::SUCCESS;
            node.real = static_cast<f64>(node.integer);
            break;
        case simdjson::dom::element_type::UINT64: {
            u64 value = 0;
            node.kind = Kind::Number;
            node.big = element.get_uint64().get(value) == simdjson::SUCCESS;
            node.real = static_cast<f64>(value);
            break;
        }
        case simdjson::dom::element_type::DOUBLE:
            node.kind = Kind::Number;
            static_cast<void>(element.get_double().get(node.real));
            break;
        case simdjson::dom::element_type::STRING:
            node.kind = Kind::String;
            static_cast<void>(element.get_string().get(node.text));
            break;
        case simdjson::dom::element_type::BOOL:
            node.kind = Kind::Boolean;
            static_cast<void>(element.get_bool().get(node.truth));
            break;
        default:
            node.kind = Kind::Null;
            break;
    }
}

// Thêm một nút cho `element`, nối nó vào array hay object ở đỉnh ngăn xếp, và mở một Frame nếu nó
// là array hay object.
Result<void> detail::Tree::Builder::add(const simdjson::dom::element element,
                                        const std::string_view key) noexcept {
    std::vector<Node>& tree = tree_->nodes;
    if (tree.size() >= kMaxValues) {
        return fail(ErrorCode::ResourceExhausted, "json: tài liệu có quá 65536 giá trị");
    }
    const auto index = static_cast<u32>(tree.size());
    tree.emplace_back().key = key;
    if (depth_ > 0) {
        Frame& parent = stack_.at(depth_ - 1);
        if (parent.last == kNone) {
            tree[parent.node].first = index;
        } else {
            tree[parent.last].next = index;
        }
        parent.last = index;
        ++tree[parent.node].size;
    }
    const simdjson::dom::element_type type = element.type();
    if (type != simdjson::dom::element_type::ARRAY && type != simdjson::dom::element_type::OBJECT) {
        fill_scalar(tree[index], element);
        return {};
    }
    // simdjson đã chặn ở kMaxDepth; kiểm lại ở đây vì ngăn xếp có cỡ cố định và dữ liệu đến từ
    // ngoài.
    if (depth_ == stack_.size()) {
        return fail(ErrorCode::ResourceExhausted, "json: lồng sâu hơn 32 cấp");
    }
    Frame& frame = stack_.at(depth_++);
    frame = Frame{};
    frame.node = index;
    frame.object = type == simdjson::dom::element_type::OBJECT;
    if (frame.object) {
        tree[index].kind = Kind::Object;
        simdjson::dom::object object;
        static_cast<void>(element.get_object().get(object));
        frame.member = object.begin();
        frame.members_end = object.end();
    } else {
        tree[index].kind = Kind::Array;
        simdjson::dom::array array;
        static_cast<void>(element.get_array().get(array));
        frame.element = array.begin();
        frame.elements_end = array.end();
    }
    return {};
}

Result<void> detail::Tree::Builder::check_unique_keys(const u32 object) noexcept {
    const std::vector<Node>& tree = tree_->nodes;
    if (tree[object].size < 2) {
        return {};
    }
    std::vector<std::string_view>& seen = tree_->keys;
    seen.clear();
    for (u32 child = tree[object].first; child != kNone; child = tree[child].next) {
        seen.push_back(tree[child].key);
    }
    std::ranges::sort(seen);
    if (std::ranges::adjacent_find(seen) != seen.end()) {
        return fail(ErrorCode::InvalidArgument, "json: object có khoá trùng");
    }
    return {};
}

Result<void> detail::Tree::Builder::build(const simdjson::dom::element root) noexcept {
    tree_->nodes.clear();
    depth_ = 0;
    if (const Result<void> added = add(root, {}); !added) {
        return added;
    }
    while (depth_ > 0) {
        Frame& top = stack_.at(depth_ - 1);
        Result<void> added;
        if (top.object && top.member != top.members_end) {
            const simdjson::dom::key_value_pair member = *top.member;
            ++top.member;
            added = add(member.value, member.key);
        } else if (!top.object && top.element != top.elements_end) {
            const simdjson::dom::element element = *top.element;
            ++top.element;
            added = add(element, {});
        } else {
            --depth_;
            added = top.object ? check_unique_keys(top.node) : Result<void>{};
        }
        if (!added) {
            return added;
        }
    }
    return {};
}

Document::Document() : tree_(std::make_unique<detail::Tree>()) {}
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;
Document::~Document() = default;

Result<Value> Document::parse(const std::string_view text) noexcept {
    ORION_ASSERT(tree_ != nullptr, "json: Document đã bị move");
    detail::Tree& tree = *tree_;
    tree.nodes.clear();
    if (text.size() > kMaxDocumentBytes) {
        return fail(ErrorCode::ResourceExhausted, "json: tài liệu lớn hơn 1 MiB",
                    static_cast<i64>(text.size()));
    }
    if (tree.parser.max_depth() != kMaxDepth || tree.parser.capacity() < text.size()) {
        const simdjson::error_code allocated =
            tree.parser.allocate(std::max(text.size(), kInitialCapacity), kMaxDepth);
        if (allocated != simdjson::SUCCESS) {
            return parse_error(allocated);
        }
    }
    simdjson::dom::element root;
    // realloc_if_needed: chép vào bộ đệm có đệm đuôi mà simdjson cần; view của bên gọi không có.
    const simdjson::error_code parsed = tree.parser.parse(text.data(), text.size(), true).get(root);
    if (parsed != simdjson::SUCCESS) {
        return parse_error(parsed);
    }
    if (const Result<void> built = detail::Tree::Builder(tree).build(root); !built) {
        tree.nodes.clear();
        return std::unexpected(built.error());
    }
    return Value(tree, 0);
}

Kind Value::kind() const noexcept {
    return node_at(tree_, index_).kind;
}

Result<bool> Value::boolean() const noexcept {
    const detail::Node& node = node_at(tree_, index_);
    if (node.kind != Kind::Boolean) {
        return wrong_kind(node.kind);
    }
    return node.truth;
}

Result<i64> Value::integer() const noexcept {
    const detail::Node& node = node_at(tree_, index_);
    if (node.kind != Kind::Number) {
        return wrong_kind(node.kind);
    }
    if (node.big) {
        return fail(ErrorCode::OutOfRange, "json: số nguyên không vừa i64");
    }
    if (!node.integral) {
        return fail(ErrorCode::InvalidArgument, "json: số không viết như số nguyên");
    }
    return node.integer;
}

Result<i64> Value::integer(const i64 min, const i64 max) const noexcept {
    const Result<i64> value = integer();
    if (value && (*value < min || *value > max)) {
        return fail(ErrorCode::OutOfRange, "json: số nguyên ngoài khoảng cho phép", *value);
    }
    return value;
}

Result<f64> Value::real() const noexcept {
    const detail::Node& node = node_at(tree_, index_);
    if (node.kind != Kind::Number) {
        return wrong_kind(node.kind);
    }
    return node.real;
}

Result<std::string_view> Value::string() const noexcept {
    const detail::Node& node = node_at(tree_, index_);
    if (node.kind != Kind::String) {
        return wrong_kind(node.kind);
    }
    return node.text;
}

Result<std::string_view> Value::string(const usize max_bytes) const noexcept {
    const Result<std::string_view> text = string();
    if (text && text->size() > max_bytes) {
        return fail(ErrorCode::OutOfRange, "json: chuỗi dài hơn giới hạn",
                    static_cast<i64>(text->size()));
    }
    return text;
}

Result<Object> Value::object() const noexcept {
    const Kind kind = this->kind();
    if (kind != Kind::Object) {
        return wrong_kind(kind);
    }
    return Object(*tree_, index_);
}

Result<Array> Value::array() const noexcept {
    const Kind kind = this->kind();
    if (kind != Kind::Array) {
        return wrong_kind(kind);
    }
    return Array(*tree_, index_);
}

Member Object::Iterator::operator*() const noexcept {
    return {.key = node_at(tree_, index_).key, .value = Value(*tree_, index_)};
}

Object::Iterator& Object::Iterator::operator++() noexcept {
    index_ = node_at(tree_, index_).next;
    return *this;
}

Object::Iterator Object::Iterator::operator++(int) noexcept {
    const Iterator before = *this;
    ++*this;
    return before;
}

usize Object::size() const noexcept {
    return node_at(tree_, index_).size;
}

Object::Iterator Object::begin() const noexcept {
    return {tree_, node_at(tree_, index_).first};
}

Object::Iterator Object::end() const noexcept {
    return {tree_, kNone};
}

std::optional<Value> Object::find(const std::string_view key) const noexcept {
    for (const Member member : *this) {
        if (member.key == key) {
            return member.value;
        }
    }
    return std::nullopt;
}

Result<Value> Object::at(const std::string_view key) const noexcept {
    const std::optional<Value> value = find(key);
    if (!value) {
        return fail(ErrorCode::InvalidArgument, "json: thiếu trường bắt buộc");
    }
    return *value;
}

Result<void> Object::expect_only(const std::span<const std::string_view> allowed) const noexcept {
    for (const Member member : *this) {
        if (std::ranges::find(allowed, member.key) == allowed.end()) {
            return fail(ErrorCode::InvalidArgument, "json: object có trường lạ");
        }
    }
    return {};
}

Value Array::Iterator::operator*() const noexcept {
    return {*tree_, index_};
}

Array::Iterator& Array::Iterator::operator++() noexcept {
    index_ = node_at(tree_, index_).next;
    return *this;
}

Array::Iterator Array::Iterator::operator++(int) noexcept {
    const Iterator before = *this;
    ++*this;
    return before;
}

usize Array::size() const noexcept {
    return node_at(tree_, index_).size;
}

Array::Iterator Array::begin() const noexcept {
    return {tree_, node_at(tree_, index_).first};
}

Array::Iterator Array::end() const noexcept {
    return {tree_, kNone};
}

// ---- Writer ----------------------------------------------------------------------------------

void Writer::before_value() noexcept {
    if (depth_ == 0) {
        ORION_ASSERT(!done_, "json: Writer đã có giá trị gốc");
        return;
    }
    Level& level = levels_.at(depth_ - 1);
    if (level.object) {
        ORION_ASSERT(level.keyed, "json: giá trị trong object phải đi sau key");
        level.keyed = false;
        return;
    }
    if (!level.empty) {
        out_ += ',';
    }
    level.empty = false;
}

void Writer::open(const bool object) noexcept {
    before_value();
    ORION_ASSERT(depth_ < kMaxDepth, "json: Writer lồng sâu hơn 32 cấp");
    levels_.at(depth_++) = Level{.object = object, .empty = true, .keyed = false};
    out_ += object ? '{' : '[';
}

void Writer::close(const bool object) noexcept {
    ORION_ASSERT(
        depth_ > 0 && levels_.at(depth_ - 1).object == object && !levels_.at(depth_ - 1).keyed,
        "json: Writer đóng sai loại hay thiếu giá trị sau key");
    --depth_;
    out_ += object ? '}' : ']';
    done_ = depth_ == 0;
}

void Writer::begin_object() noexcept {
    open(true);
}

void Writer::end_object() noexcept {
    close(true);
}

void Writer::begin_array() noexcept {
    open(false);
}

void Writer::end_array() noexcept {
    close(false);
}

void Writer::key(const std::string_view name) noexcept {
    ORION_ASSERT(depth_ > 0 && levels_.at(depth_ - 1).object && !levels_.at(depth_ - 1).keyed,
                 "json: key chỉ đứng trong object, trước giá trị");
    Level& level = levels_.at(depth_ - 1);
    if (!level.empty) {
        out_ += ',';
    }
    level.empty = false;
    level.keyed = true;
    append_string(name);
    out_ += ':';
}

// Escape theo RFC 8259, như log của engine/core: byte UTF-8 hỏng thành U+FFFD.
void Writer::append_string(const std::string_view text) noexcept {
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
    constexpr std::string_view kHex = "0123456789abcdef";
    out_ += '"';
    usize i = 0;
    while (i < text.size()) {
        const usize length = core::utf8_sequence_length(text, i);
        if (length != 1) {
            out_ += length == 0 ? kReplacement : text.substr(i, length);
            i += std::max<usize>(length, 1);
            continue;
        }
        const char c = text[i++];
        switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (static_cast<u8>(c) < 0x20U) {
                    out_ += "\\u00";
                    out_ += kHex[static_cast<u8>(c) >> 4U];
                    out_ += kHex[static_cast<u8>(c) & 0xFU];
                } else {
                    out_ += c;
                }
        }
    }
    out_ += '"';
}

void Writer::string(const std::string_view value) noexcept {
    before_value();
    append_string(value);
    done_ = depth_ == 0;
}

void Writer::integer(const i64 value) noexcept {
    before_value();
    std::array<char, 24> digits{};
    const std::to_chars_result written =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out_.append(digits.data(), written.ptr);
    done_ = depth_ == 0;
}

void Writer::real(const f64 value) noexcept {
    ORION_ASSERT(std::isfinite(value), "json: JSON không có NaN hay vô cực");
    before_value();
    std::array<char, 32> digits{};
    const std::to_chars_result written =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    const std::string_view text(digits.data(), written.ptr);
    out_ += text;
    // Dạng ngắn nhất của 1.0 là "1": thêm ".0" để bên đọc vẫn thấy một số thực.
    if (text.find_first_of(".eE") == std::string_view::npos) {
        out_ += ".0";
    }
    done_ = depth_ == 0;
}

void Writer::boolean(const bool value) noexcept {
    before_value();
    out_ += value ? "true" : "false";
    done_ = depth_ == 0;
}

void Writer::null() noexcept {
    before_value();
    out_ += "null";
    done_ = depth_ == 0;
}

std::string Writer::finish() noexcept {
    ORION_ASSERT(done_ && depth_ == 0, "json: Writer chưa có đúng một giá trị gốc hoàn chỉnh");
    std::string out = std::move(out_);
    out_.clear();
    done_ = false;
    return out;
}

}  // namespace orion::http::json
