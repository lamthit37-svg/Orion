#pragma once

// JSON của dịch vụ HTTP (ARCH §4.6; CLAUDE.md X.14; ADR 0014): đọc bằng simdjson qua API mã lỗi,
// ghi bằng Writer tự viết. Hợp đồng ở docs/formats/http.md, mục "JSON".
//
// - Body JSON là dữ liệu từ ngoài (X.5): sai cú pháp, sai UTF-8, sai kiểu hay ngoài khoảng đều là
//   Error trả về, không bao giờ là assert. Document::parse là parser của dữ liệu từ ngoài, có fuzz
//   target tests/fuzz/json_document.cpp (X.4).
// - Document chép cây của simdjson sang một mảng nút của chính nó, nên header không lộ kiểu nào của
//   simdjson (X.2), và từ chối những thứ simdjson cho qua mà một parser request không nên nhận:
//   object có khoá trùng (hai parser đọc cùng body có thể chọn hai giá trị khác nhau), lồng quá
//   sâu, quá nhiều giá trị.
// - Luồng: Document, Value, Object, Array, Writer không đồng bộ; mỗi đối tượng thuộc một luồng.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace orion::http::json {

// Độ sâu lồng tối đa của array và object.
inline constexpr u32 kMaxDepth = 32;
// Số giá trị tối đa của một tài liệu, tính mọi giá trị ở mọi độ sâu.
inline constexpr usize kMaxValues = usize{1} << 16U;
// Cỡ tối đa của một tài liệu. Body HTTP còn bị giới hạn chặt hơn ở tầng trên.
inline constexpr usize kMaxDocumentBytes = usize{1} << 20U;

enum class Kind : u8 {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

class Document;
class Object;
class Array;

namespace detail {
// Cây của một lần parse (json.cpp), Document giữ trên heap. Value, Object, Array trỏ vào cây chứ
// không vào Document, nên move Document không làm cũ chúng.
class Tree;
}  // namespace detail

// Một giá trị trong Document. Rẻ để chép; sống như cây của nó (xem Document).
class Value {
public:
    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool is_null() const noexcept { return kind() == Kind::Null; }

    // Lỗi của mọi accessor: InvalidArgument khi giá trị có kiểu khác (detail là Kind của nó).
    [[nodiscard]] Result<bool> boolean() const noexcept;
    // Số nguyên vừa i64. Lỗi thêm: InvalidArgument với số viết có phần thập phân hay số mũ ("5.0",
    // "5e0"), OutOfRange với số nguyên không vừa i64.
    [[nodiscard]] Result<i64> integer() const noexcept;
    // Như integer, và OutOfRange khi ngoài [min, max] (detail là giá trị).
    [[nodiscard]] Result<i64> integer(i64 min, i64 max) const noexcept;
    // Mọi số, đổi sang f64: số nguyên lớn hơn 2^53 có thể mất độ chính xác.
    [[nodiscard]] Result<f64> real() const noexcept;
    // UTF-8 hợp lệ (simdjson đã kiểm); có thể chứa U+0000. View sống như Value.
    [[nodiscard]] Result<std::string_view> string() const noexcept;
    // Như string, và OutOfRange khi dài hơn `max_bytes` byte (detail là độ dài).
    [[nodiscard]] Result<std::string_view> string(usize max_bytes) const noexcept;
    [[nodiscard]] Result<Object> object() const noexcept;
    [[nodiscard]] Result<Array> array() const noexcept;

private:
    friend class Document;
    friend class Object;
    friend class Array;
    Value(const detail::Tree& tree, u32 index) noexcept : tree_(&tree), index_(index) {}

    const detail::Tree* tree_;
    u32 index_;
};

struct Member {
    std::string_view key;
    Value value;
};

class Object {
public:
    // Duyệt thành viên theo thứ tự trong tài liệu.
    class Iterator {
    public:
        using value_type = Member;
        using difference_type = std::ptrdiff_t;

        Iterator() noexcept = default;
        [[nodiscard]] Member operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept;
        friend bool operator==(const Iterator&, const Iterator&) noexcept = default;

    private:
        friend class Object;
        Iterator(const detail::Tree* tree, u32 index) noexcept : tree_(tree), index_(index) {}

        const detail::Tree* tree_ = nullptr;
        u32 index_ = 0;
    };

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;
    // Thành viên tên `key`; nullopt khi không có. O(số thành viên).
    [[nodiscard]] std::optional<Value> find(std::string_view key) const noexcept;
    // Thành viên bắt buộc. Lỗi: InvalidArgument khi không có.
    [[nodiscard]] Result<Value> at(std::string_view key) const noexcept;
    // Lỗi InvalidArgument khi object có thành viên không nằm trong `allowed`: parser request từ
    // chối trường lạ, để client gõ sai tên trường thấy lỗi thay vì bị bỏ qua lặng lẽ.
    [[nodiscard]] Result<void> expect_only(
        std::span<const std::string_view> allowed) const noexcept;

private:
    friend class Value;
    Object(const detail::Tree& tree, u32 index) noexcept : tree_(&tree), index_(index) {}

    const detail::Tree* tree_;
    u32 index_;
};

class Array {
public:
    // Duyệt phần tử theo thứ tự.
    class Iterator {
    public:
        using value_type = Value;
        using difference_type = std::ptrdiff_t;

        Iterator() noexcept = default;
        [[nodiscard]] Value operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept;
        friend bool operator==(const Iterator&, const Iterator&) noexcept = default;

    private:
        friend class Array;
        Iterator(const detail::Tree* tree, u32 index) noexcept : tree_(tree), index_(index) {}

        const detail::Tree* tree_ = nullptr;
        u32 index_ = 0;
    };

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;

private:
    friend class Value;
    Array(const detail::Tree& tree, u32 index) noexcept : tree_(&tree), index_(index) {}

    const detail::Tree* tree_;
    u32 index_;
};

// Một tài liệu JSON đã parse và kiểm. Sở hữu bộ nhớ của parser và của cây. Mọi Value, Object, Array
// và string_view lấy từ một lần parse sống tới lần parse kế tiếp, hay tới khi cây bị huỷ (Document
// giữ nó bị huỷ, hay bị gán đè). Move chuyển cây sang Document mới mà không làm cũ chúng; Document
// đã bị move chỉ được huỷ hay gán lại.
class Document {
public:
    Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document();

    // Parse `text` (RFC 8259, UTF-8). Lỗi: InvalidArgument (sai cú pháp, sai UTF-8, khoá trùng
    // trong một object, nội dung thừa sau giá trị gốc; detail là mã lỗi của simdjson khi có),
    // OutOfRange (số nguyên không vừa 64 bit), ResourceExhausted (lớn hơn kMaxDocumentBytes, sâu
    // hơn kMaxDepth, nhiều hơn kMaxValues giá trị).
    [[nodiscard]] Result<Value> parse(std::string_view text) noexcept;

private:
    std::unique_ptr<detail::Tree> tree_;
};

// Ghi JSON gọn (không khoảng trắng). Cấu trúc sai (khoá ngoài object, đóng sai loại, thiếu giá trị
// gốc, lồng sâu hơn kMaxDepth) là lỗi lập trình. Chuỗi được escape theo RFC 8259; byte UTF-8 hỏng
// thành U+FFFD như log của engine/core, nên kết quả luôn là JSON hợp lệ.
class Writer {
public:
    void begin_object() noexcept;
    void end_object() noexcept;
    void begin_array() noexcept;
    void end_array() noexcept;
    // Tên thành viên kế tiếp; chỉ trong object, trước mỗi giá trị.
    void key(std::string_view name) noexcept;
    void string(std::string_view value) noexcept;
    void integer(i64 value) noexcept;
    // Tiền điều kiện: hữu hạn (JSON không có NaN, vô cực). Ghi dạng ngắn nhất đọc lại đúng từng
    // bit.
    void real(f64 value) noexcept;
    void boolean(bool value) noexcept;
    void null() noexcept;

    // Tiền điều kiện: đúng một giá trị gốc, mọi object, array đã đóng.
    [[nodiscard]] std::string finish() noexcept;

private:
    struct Level {
        bool object = false;
        bool empty = true;
        bool keyed = false;
    };

    void before_value() noexcept;
    void open(bool object) noexcept;
    void close(bool object) noexcept;
    void append_string(std::string_view text) noexcept;

    std::string out_;
    std::array<Level, kMaxDepth> levels_{};
    u32 depth_ = 0;
    bool done_ = false;
};

}  // namespace orion::http::json
