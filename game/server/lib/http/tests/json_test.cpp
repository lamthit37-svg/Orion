#include "game/server/lib/http/json.hpp"

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orion::http::json {
namespace {

[[nodiscard]] std::string why(const Error& error) {
    return std::format("{}", error);
}

// Mã lỗi khi parse `text`; ErrorCode{} (0) khi parse được.
[[nodiscard]] ErrorCode parse_code(const std::string_view text) {
    Document document;
    const Result<Value> root = document.parse(text);
    return root ? ErrorCode{} : root.error().code();
}

TEST(Json, ReadsEveryKindOfValue) {
    Document document;
    const Result<Value> root = document.parse(
        R"({"name": "Hà Nội ☃", "level": 42, "ratio": 0.25, "ok": true, "none": null,
            "tags": ["a", "b"], "nested": {"x": -7}})");
    ASSERT_TRUE(root.has_value()) << why(root.error());
    const Result<Object> object = root->object();
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(object->size(), 7U);

    EXPECT_EQ(object->at("name").and_then([](const Value v) { return v.string(); }),
              std::string_view("Hà Nội ☃"));
    EXPECT_EQ(object->at("level").and_then([](const Value v) { return v.integer(); }), 42);
    EXPECT_EQ(object->at("ratio").and_then(&Value::real), 0.25);
    EXPECT_EQ(object->at("ok").and_then(&Value::boolean), true);
    const Result<Value> none = object->at("none");
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->is_null());

    const Result<Array> tags = object->at("tags").and_then(&Value::array);
    ASSERT_TRUE(tags.has_value());
    std::vector<std::string_view> seen;
    for (const Value tag : *tags) {
        const Result<std::string_view> text = tag.string();
        ASSERT_TRUE(text.has_value());
        seen.push_back(*text);
    }
    EXPECT_EQ(seen, (std::vector<std::string_view>{"a", "b"}));
    EXPECT_EQ(tags->size(), 2U);

    const Result<Object> nested = object->at("nested").and_then(&Value::object);
    ASSERT_TRUE(nested.has_value());
    EXPECT_EQ(nested->at("x").and_then([](const Value v) { return v.integer(); }), -7);
}

TEST(Json, MembersKeepDocumentOrder) {
    Document document;
    const Result<Value> root = document.parse(R"({"z": 1, "a": 2, "m": {}, "b": []})");
    ASSERT_TRUE(root.has_value());
    const Result<Object> object = root->object();
    ASSERT_TRUE(object.has_value());
    std::string keys;
    for (const Member member : *object) {
        keys += member.key;
    }
    EXPECT_EQ(keys, "zamb");
    const Result<Object> empty_object =
        object->at("m").and_then([](const Value v) { return v.object(); });
    ASSERT_TRUE(empty_object.has_value());
    EXPECT_EQ(empty_object->size(), 0U);
    EXPECT_EQ(empty_object->begin(), empty_object->end());
    EXPECT_FALSE(object->find("q").has_value());
}

TEST(Json, AccessorsRefuseTheWrongKind) {
    Document document;
    const Result<Value> root = document.parse(R"({"s": "x", "n": 1, "b": false, "a": []})");
    ASSERT_TRUE(root.has_value());
    const Result<Object> object = root->object();
    ASSERT_TRUE(object.has_value());
    const Result<Value> s = object->at("s");
    const Result<Value> n = object->at("n");
    const Result<Value> b = object->at("b");
    const Result<Value> a = object->at("a");
    ASSERT_TRUE(s && n && b && a);
    EXPECT_EQ(s->integer().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(s->array().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(n->string().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(n->string().error().detail(), static_cast<i64>(Kind::Number));
    EXPECT_EQ(b->object().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(b->real().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(a->boolean().error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(a->integer(0, 1).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(a->string(10).error().code(), ErrorCode::InvalidArgument);
}

TEST(Json, IntegersMustBeWrittenAsIntegersAndFit) {
    Document document;
    const auto integer = [&](const std::string_view text) {
        const Result<Value> root = document.parse(text);
        EXPECT_TRUE(root.has_value()) << text;
        return root ? root->integer() : Result<i64>(0);
    };
    EXPECT_EQ(integer("-9223372036854775808"), std::numeric_limits<i64>::min());
    EXPECT_EQ(integer("9223372036854775807"), std::numeric_limits<i64>::max());
    EXPECT_EQ(integer("-0"), 0);
    EXPECT_EQ(integer("9223372036854775808").error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(integer("18446744073709551615").error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(integer("5.0").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(integer("5e0").error().code(), ErrorCode::InvalidArgument);
    // Ngoài 64 bit thì simdjson từ chối cả tài liệu.
    EXPECT_EQ(parse_code("-9223372036854775809"), ErrorCode::OutOfRange);

    const Result<Value> ranged = document.parse("300");
    ASSERT_TRUE(ranged.has_value());
    EXPECT_EQ(ranged->integer(0, 300), 300);
    const Result<i64> above = ranged->integer(0, 299);
    ASSERT_FALSE(above.has_value());
    EXPECT_EQ(above.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(above.error().detail(), 300);
    EXPECT_EQ(ranged->integer(301, 400).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(ranged->real(), 300.0);
}

TEST(Json, StringsAreUtf8AndBounded) {
    Document document;
    const Result<Value> root =
        document.parse(R"(["é\u00e9", "\ud83d\ude00", "a\u0000b", "tab\t"])");
    ASSERT_TRUE(root.has_value()) << why(root.error());
    const Result<Array> array = root->array();
    ASSERT_TRUE(array.has_value());
    std::vector<std::string> strings;
    for (const Value value : *array) {
        const Result<std::string_view> text = value.string();
        ASSERT_TRUE(text.has_value());
        strings.emplace_back(*text);
    }
    ASSERT_EQ(strings.size(), 4U);
    EXPECT_EQ(strings[0], "éé");
    EXPECT_EQ(strings[1], "😀");
    EXPECT_EQ(strings[2], std::string("a\0b", 3));
    EXPECT_EQ(strings[3], "tab\t");

    const Result<Value> name = document.parse(R"("abcdé")");
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name->string(6), std::string_view("abcdé"));
    const Result<std::string_view> longer = name->string(5);
    ASSERT_FALSE(longer.has_value());
    EXPECT_EQ(longer.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(longer.error().detail(), 6);
}

TEST(Json, RequestParsersRefuseMissingAndUnknownFields) {
    Document document;
    const Result<Value> root = document.parse(R"({"email": "a@b.c", "pasword": "x"})");
    ASSERT_TRUE(root.has_value());
    const Result<Object> object = root->object();
    ASSERT_TRUE(object.has_value());
    const std::array<std::string_view, 2> fields{"email", "password"};
    const Result<void> only = object->expect_only(fields);
    ASSERT_FALSE(only.has_value());
    EXPECT_EQ(only.error().code(), ErrorCode::InvalidArgument);
    const Result<Value> missing = object->at("password");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
    const std::array<std::string_view, 3> wider{"email", "password", "pasword"};
    EXPECT_TRUE(object->expect_only(wider).has_value());
}

// Tài liệu sai hay quá cỡ: lỗi có mã, không bao giờ assert (CLAUDE.md X.5).
TEST(Json, MalformedAndOversizedDocumentsAreErrors) {
    const std::string depth_32 = std::string(kMaxDepth, '[') + std::string(kMaxDepth, ']');
    const std::string depth_33 = '[' + depth_32 + ']';
    EXPECT_EQ(parse_code(depth_32), ErrorCode{});
    EXPECT_EQ(parse_code(depth_33), ErrorCode::ResourceExhausted);

    std::string wide = "[";
    for (usize i = 0; i < kMaxValues - 1; ++i) {
        wide += i == 0 ? "0" : ",0";
    }
    wide += ']';
    EXPECT_EQ(parse_code(wide), ErrorCode{});
    wide.insert(1, "0,");
    EXPECT_EQ(parse_code(wide), ErrorCode::ResourceExhausted);

    std::string huge(kMaxDocumentBytes + 1, ' ');
    huge.front() = '0';
    EXPECT_EQ(parse_code(huge), ErrorCode::ResourceExhausted);

    for (const std::string_view bad :
         {std::string_view{""}, std::string_view{"{"}, std::string_view{"{} x"},
          std::string_view{R"({"a":1,"a":2})"}, std::string_view{R"({"a":{"b":1,"b":1}})"},
          std::string_view{"[01]"}, std::string_view{R"("\ud800")"}, std::string_view{"'a'"},
          std::string_view{R"({"a" 1})"}, std::string_view{"[1,]"}, std::string_view{"nul"},
          std::string_view{"\"a\x01\""}, std::string_view{"\"\xC3\x28\""}}) {
        const ErrorCode code = parse_code(bad);
        EXPECT_NE(code, ErrorCode{}) << bad;
        EXPECT_TRUE(code == ErrorCode::InvalidArgument || code == ErrorCode::OutOfRange) << bad;
    }
    // RFC 8259 cho phép bỏ qua BOM đầu tài liệu; simdjson bỏ qua.
    EXPECT_EQ(parse_code("\xEF\xBB\xBF{}"), ErrorCode{});
    // Khoá trùng ở hai object khác nhau thì không phải trùng.
    EXPECT_EQ(parse_code(R"([{"a":1},{"a":2}])"), ErrorCode{});
}

// Parse lại làm cũ mọi Value trước đó; lần parse lỗi để Document rỗng, không giữ cây dở dang.
TEST(Json, ADocumentCanBeReused) {
    Document document;
    ASSERT_TRUE(document.parse(R"({"a": 1})").has_value());
    ASSERT_FALSE(document.parse("[1, 2").has_value());
    const Result<Value> again = document.parse(R"(["x"])");
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->kind(), Kind::Array);
    Document moved = std::move(document);
    const Result<Value> after = moved.parse("true");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->boolean(), true);
}

// Move, bằng dựng hay bằng gán, chuyển cây sang Document mới: Value, Object và chuỗi lấy từ trước
// vẫn đọc được. Dưới ASan, một view còn trỏ vào Document cũ sẽ là lỗi use-after-free.
TEST(Json, ValuesSurviveMovingTheDocument) {
    std::vector<Document> documents(1);
    const Result<Value> root = documents.front().parse(R"({"name": "Hà Nội", "list": [1, 2]})");
    ASSERT_TRUE(root.has_value());
    const Result<Object> object = root->object();
    ASSERT_TRUE(object.has_value());
    const Result<std::string_view> name =
        object->at("name").and_then([](const Value v) { return v.string(); });
    ASSERT_TRUE(name.has_value());

    // Nới vector nhiều lần: mỗi lần các Document bị move sang vùng nhớ mới.
    for (usize i = 0; i < 64; ++i) {
        documents.emplace_back();
    }
    Document moved = std::move(documents.front());
    documents.clear();
    Document assigned;
    assigned = std::move(moved);

    EXPECT_EQ(*name, "Hà Nội");
    EXPECT_EQ(object->at("name").and_then([](const Value v) { return v.string(); }),
              std::string_view("Hà Nội"));
    const Result<Array> list = object->at("list").and_then(&Value::array);
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 2U);
}

TEST(JsonWriter, WritesCompactJson) {
    Writer writer;
    writer.begin_object();
    writer.key("name");
    writer.string("Hà Nội");
    writer.key("level");
    writer.integer(-42);
    writer.key("ok");
    writer.boolean(true);
    writer.key("none");
    writer.null();
    writer.key("list");
    writer.begin_array();
    writer.integer(1);
    writer.begin_object();
    writer.end_object();
    writer.begin_array();
    writer.end_array();
    writer.end_array();
    writer.end_object();
    EXPECT_EQ(writer.finish(),
              R"({"name":"Hà Nội","level":-42,"ok":true,"none":null,"list":[1,{},[]]})");
    // Writer dùng lại được sau finish.
    writer.integer(std::numeric_limits<i64>::min());
    EXPECT_EQ(writer.finish(), "-9223372036854775808");
}

TEST(JsonWriter, EscapesStringsAndReplacesBrokenUtf8) {
    Writer writer;
    writer.string("q\"b\\n\n r\r t\t c\x01 d\x1f e\x7f ok");
    EXPECT_EQ(writer.finish(), R"("q\"b\\n\n r\r t\t c\u0001 d\u001f e)"
                               "\x7f"
                               R"( ok")");
    writer.string(std::string("a\0b", 3));
    EXPECT_EQ(writer.finish(), R"("a\u0000b")");
    // Byte hỏng và điểm mã dở dang đều thành U+FFFD; chuỗi ra vẫn đọc lại được.
    writer.string("x\xC3\x28y\xE2\x82");
    const std::string broken = writer.finish();
    EXPECT_EQ(broken, "\"x\xEF\xBF\xBD(y\xEF\xBF\xBD\xEF\xBF\xBD\"");
    Document document;
    EXPECT_TRUE(document.parse(broken).has_value());
}

TEST(JsonWriter, RealsStayRealAndRoundTrip) {
    Document document;
    for (const f64 value : {0.0, -0.0, 1.0, -2.5, 0.1, 1e21, 5e-324, 1.7976931348623157e308}) {
        Writer writer;
        writer.real(value);
        const std::string text = writer.finish();
        const Result<Value> parsed = document.parse(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        const Result<f64> read = parsed->real();
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(std::bit_cast<u64>(*read), std::bit_cast<u64>(value)) << text;
        // Không thành số nguyên khi đọc lại.
        EXPECT_EQ(parsed->integer().error().code(), ErrorCode::InvalidArgument) << text;
    }
}

TEST(JsonWriterDeathTest, StructuralMisuseIsAProgrammingError) {
    EXPECT_DEATH(
        {
            Writer writer;
            writer.key("outside");
        },
        "key chỉ đứng trong object");
    EXPECT_DEATH(
        {
            Writer writer;
            writer.begin_array();
            writer.end_object();
        },
        "đóng sai loại");
    EXPECT_DEATH(
        {
            Writer writer;
            writer.begin_object();
            const std::string unfinished = writer.finish();
            EXPECT_TRUE(unfinished.empty());
        },
        "chưa có đúng một giá trị gốc");
    EXPECT_DEATH(
        {
            Writer writer;
            writer.real(std::numeric_limits<f64>::quiet_NaN());
        },
        "NaN");
}

}  // namespace
}  // namespace orion::http::json
