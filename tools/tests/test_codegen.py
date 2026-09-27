"""Test codegen của protocol (tools/codegen; docs/formats/protocol.md): mỗi loại lỗi schema có một
test (ADR 0004, mục Hệ quả), mô hình tính đúng cỡ, và code sinh ra tất định. Khứ hồi mã hoá rồi giải
mã được test bằng C++ trong game/shared/tests/protocol_codegen_test.cpp."""

from __future__ import annotations

import contextlib
import io
import pathlib
import tempfile
import unittest

from codegen import cpp
from codegen import orion_codegen
from codegen import schema

HEADER = "version 1\n"


def message(fields: str = "", attributes: str | None = None, name: str = "Hello",
            id_: int = 1) -> str:
    attrs = ("channel reliable_ordered\n    from client\n    rate 5/s burst 10"
             if attributes is None else attributes)
    return f"message {name} = {id_} {{\n    {attrs}\n    {fields}\n}}\n"


class ErrorTest(unittest.TestCase):
    """Mỗi luật của protocol.md có ít nhất một schema sai tương ứng."""

    def fails(self, text: str, expected: str, line: int | None = None,
              sources: dict[str, str] | None = None) -> schema.SchemaError:
        with self.assertRaises(schema.SchemaError) as caught:
            schema.loads(sources if sources is not None else {"a.schema": text})
        error = caught.exception
        self.assertIn(expected, error.message)
        if line is not None:
            self.assertEqual(error.location.line, line, error.format())
        self.assertRegex(error.format(), r"^[^:]+:\d+:\d+: lỗi \[schema\]: ")
        return error

    def test_lexical_errors(self) -> None:
        self.fails(HEADER + "message A$ = 1 {}", "ký tự lạ", line=2)
        self.fails(HEADER + "struct", "cần tên kiểu, gặp hết tệp")
        self.fails(HEADER + "banana", "cần version, enum, struct, message hay event")
        self.fails(HEADER + "7", "cần version, enum, struct, message hay event")

    def test_version_rules(self) -> None:
        self.fails(message(), "thiếu khai báo version")
        self.fails(HEADER + "version 2\n" + message(), "version khai hơn một lần", line=2)
        self.fails("version 0\n" + message(), "version 0 phải từ 1")
        self.fails(f"version {2**32}\n" + message(), "phải từ 1 tới 4294967295")
        self.fails("version 1.5\n" + message(), "version là số nguyên")

    def test_names(self) -> None:
        self.fails(HEADER + message(name="hello"), "phải viết CamelCase")
        self.fails(HEADER + message(name="HTTP"), "phải viết CamelCase")
        self.fails(HEADER + message(name="Tick"), "đã được code hỗ trợ dùng")
        self.fails(HEADER + message(name="Event"), "đã được code hỗ trợ dùng")
        self.fails(HEADER + message("Bad: bool"), "phải viết snake_case")
        self.fails(HEADER + message("two__parts: bool"), "phải viết snake_case")
        self.fails(HEADER + message("class: bool"), "là từ khoá của C++")
        self.fails(HEADER + message("errno: bool"), "là từ khoá của C++")
        self.fails(HEADER + message("u32: bool"), "trùng kiểu số của engine/core/types.hpp")
        self.fails(HEADER + message("a: bool\n    a: bool"), "trường 'a' trùng")
        self.fails(HEADER + message() + message(id_=2), "tên kiểu 'Hello' trùng")
        # Tên thuộc tính của schema được dùng làm tên trường.
        protocol = schema.loads({"a.schema": HEADER + message("channel: bool\n    from: bool")})
        self.assertEqual([f.name for f in protocol.messages[0].fields], ["channel", "from"])

    def test_enum_rules(self) -> None:
        used = message("value: Kind")
        self.fails(HEADER + "enum Kind : i8 { A = 1 }\n" + used, "kiểu nền phải là u8")
        self.fails(HEADER + "enum Kind : u8 {}\n" + used, "không có giá trị nào")
        self.fails(HEADER + "enum Kind : u8 { Aa = 256 }\n" + used, "không vừa kiểu nền u8")
        self.fails(HEADER + "enum Kind : u8 { Aa = -1 }\n" + used, "không vừa kiểu nền u8")
        self.fails(HEADER + "enum Kind : u8 { Aa = 1 Aa = 2 }\n" + used, "giá trị enum 'Aa' trùng")
        self.fails(HEADER + "enum Kind : u8 { Aa = 1 Bb = 1 }\n" + used, "số của giá trị enum")
        self.fails(HEADER + "enum Kind : u8 { aa = 1 }\n" + used, "phải viết CamelCase")
        self.fails(HEADER + "enum Kind : u8 { Aa 1 }\n" + used, "cần '='")

    def test_struct_rules(self) -> None:
        self.fails(HEADER + "struct Empty {}\n" + message("e: Empty"), "không có trường nào")
        self.fails(HEADER + "struct Loop { next: array[2] of Loop }\n" + message("l: Loop"),
                   "chứa chính nó")
        self.fails(HEADER + "struct Aa { b: Bb }\nstruct Bb { a: Aa }\n" + message("a: Aa"),
                   "chứa chính nó")
        self.fails(HEADER + message("x: Missing"), "không phải enum hay struct đã khai")
        self.fails(HEADER + message(name="Other", id_=2) + message("x: Other"),
                   "không phải enum hay struct đã khai")

    def test_unused_declarations(self) -> None:
        self.fails(HEADER + "enum Kind : u8 { Aa = 1 }\n" + message(), "'Kind' không được")
        self.fails(HEADER + "struct Pair { a: bool }\n" + message(), "'Pair' không được")

    def test_message_attributes(self) -> None:
        self.fails(HEADER + message(attributes="from client\n    rate 1/s burst 1"),
                   "thiếu thuộc tính 'channel'")
        self.fails(HEADER + message(attributes="channel unreliable\n    rate 1/s burst 1"),
                   "thiếu thuộc tính 'from'")
        self.fails(HEADER + message(attributes="channel unreliable\n    from client"),
                   "thiếu thuộc tính 'rate'")
        self.fails(HEADER + message(attributes="channel unreliable\n    channel unreliable"),
                   "khai hai lần")
        self.fails(HEADER + message(attributes="stream economy"), "thuộc tính lạ 'stream'")
        self.fails(HEADER + message(attributes="channel fast"), "kênh lạ 'fast'")
        self.fails(HEADER + message(attributes="channel unreliable\n    from gateway"),
                   "bên gửi phải là client hay server")
        base = "channel unreliable\n    from client\n    "
        self.fails(HEADER + message(attributes=base + "rate 0/s burst 1"), "tốc độ 0 phải từ 1")
        self.fails(HEADER + message(attributes=base + f"rate {10**9 + 1}/s burst 1"),
                   "tốc độ")
        self.fails(HEADER + message(attributes=base + "rate 1/s burst 0"), "burst 0 phải từ 1")
        self.fails(HEADER + message(attributes=base + "rate 1/s burst 1000001"), "burst")
        self.fails(HEADER + message(attributes=base + "rate 1/m burst 1"), "cần 's'")
        # Thuộc tính đứng sau trường bị đọc như một trường.
        self.fails(HEADER + message("x: bool\n    channel unreliable"),
                   "cần ':', gặp 'unreliable'")

    def test_ids(self) -> None:
        self.fails(HEADER + message(id_=0), "id 0 phải từ 1")
        self.fails(HEADER + message(id_=65536), "phải từ 1 tới 65535")
        self.fails(HEADER + message() + message(name="Other"), "id của message '1' trùng")
        self.fails(HEADER + "event Aa = 1 { stream s }\nevent Bb = 1 { stream s }\n",
                   "id của event '1' trùng")
        # Message và event có hai nhóm id riêng.
        schema.loads({"a.schema": HEADER + message() + "event Aa = 1 { stream s }\n"})

    def test_event_attributes(self) -> None:
        self.fails(HEADER + "event Aa = 1 { x: bool }\n", "thiếu thuộc tính 'stream'")
        self.fails(HEADER + "event Aa = 1 { stream Economy }\n", "phải viết snake_case")
        self.fails(HEADER + "event Aa = 1 { channel unreliable }\n", "thuộc tính lạ 'channel'")

    def test_type_rules(self) -> None:
        self.fails(HEADER + message("x: bits(0)"), "số bit phải từ 1 tới 64")
        self.fails(HEADER + message("x: bits(65)"), "số bit phải từ 1 tới 64")
        self.fails(HEADER + message("x: int[5, 4]"), "cần min ≤ max")
        self.fails(HEADER + message(f"x: int[0, {2**63}]"), "cả hai trong i64")
        self.fails(HEADER + message("x: int[0.5, 4]"), "min là số nguyên")
        self.fails(HEADER + message("x: quantized[1, 1, 0.1]"), "cần min < max và step > 0")
        self.fails(HEADER + message("x: quantized[0, 1, 0]"), "cần min < max và step > 0")
        self.fails(HEADER + message("x: quantized[0, 1, -1]"), "cần min < max và step > 0")
        self.fails(HEADER + message("x: quantized[-1" + "0" * 308 + ", 1" + "0" * 308 + ", 1]"),
                   "mọi số phải hữu hạn")
        self.fails(HEADER + message("x: quantized[0, 1, 0.0000000001]"), "phải từ 1 tới 2^32")
        self.fails(HEADER + message("x: bytes[0]"), "cỡ phải từ 1 tới 65535")
        self.fails(HEADER + message("x: string[65536]"), "cỡ phải từ 1 tới 65535")
        self.fails(HEADER + message("x: array[0] of bool"), "cỡ phải từ 1 tới 65535")
        self.fails(HEADER + message("x: array[2] bool"), "cần 'of'")
        self.fails(HEADER + message("x: float"), "kiểu lạ 'float'")

    def test_size_limits(self) -> None:
        unreliable = "channel unreliable\n    from client\n    rate 1/s burst 1"
        self.fails(HEADER + message("x: bytes[1149]", attributes=unreliable), "vượt 1150 byte")
        schema.loads({"a.schema": HEADER + message("x: bytes[1147]", attributes=unreliable)})
        self.fails(HEADER + message("x: array[40] of bytes[1000]"), "vượt 32768 byte")
        self.fails(HEADER + "event Aa = 1 { stream s\n x: array[40] of bytes[1000] }\n",
                   "vượt 32768 byte của event")

    def test_errors_name_the_file(self) -> None:
        error = self.fails("", "cần tên kiểu", sources={"a.schema": HEADER + message(),
                                                          "b.schema": "enum"})
        self.assertEqual(error.location.path, "b.schema")

    def test_files_must_be_utf8(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "bad.schema"
            path.write_bytes(b"version 1\n# \xff\n")
            with self.assertRaises(schema.SchemaError) as caught:
                schema.load([path], pathlib.Path(tmp))
            self.assertIn("không phải UTF-8", caught.exception.message)
            self.assertEqual(caught.exception.location.path, "bad.schema")


class ModelTest(unittest.TestCase):
    def test_sizes_follow_the_wire_format(self) -> None:
        protocol = schema.loads({"a.schema": HEADER + """
            enum Kind : u8 { Aa = 3 Bb = 9 }
            struct Pair { a: bool b: int[-8, 7] }
            message Big = 40 {
                channel reliable_ordered
                from server
                rate 1/s burst 1
                kind: Kind
                pairs: array[5] of Pair
                name: string[10]
                q: quantized[-1, 1, 0.25]
                t: tick
                s: short_tick
                r: replicated_id
                bits: bits(5)
            }
            event Done = 3 { stream audit }
        """})
        big = protocol.messages[0]
        bits = {f.name: protocol.max_bits(f.type) for f in big.fields}
        self.assertEqual(bits, {"kind": 3, "pairs": 3 + 5 * (1 + 4), "name": 4 + 80, "q": 4,
                                "t": 64, "s": 16, "r": 32, "bits": 5})
        # id: 6 bit cho id lớn nhất 40.
        self.assertEqual(protocol.max_encoded_size(big), (6 + sum(bits.values()) + 7) // 8)
        self.assertEqual(protocol.max_encoded_size(protocol.events[0]), 1)
        self.assertEqual(protocol.max_message_id, 40)
        self.assertEqual(protocol.max_event_id, 3)

    def test_quantization_matches_the_bitstream(self) -> None:
        quantized = schema.QuantizedType(0.0, 6.2832, 0.01, ("0", "6.2832", "0.01"))
        self.assertEqual(quantized.max_index, 629)

    def test_schema_text_matches_the_source(self) -> None:
        texts = ["bool", "bits(5)", "int[-3, 4]", "quantized[0, 1.5, 0.25]", "bytes[4]",
                 "string[9]", "tick", "short_tick", "replicated_id",
                 "array[2] of array[3] of Kind"]
        fields = "\n    ".join(f"f{i}: {text}" for i, text in enumerate(texts))
        protocol = schema.loads({"a.schema": HEADER + "enum Kind : u8 { Aa = 1 }\n"
                                 + message(fields)})
        self.assertEqual([cpp.schema_text(f.type) for f in protocol.messages[0].fields], texts)

    def test_int_types(self) -> None:
        self.assertEqual(cpp.int_type(0, 255), "u8")
        self.assertEqual(cpp.int_type(0, 256), "u16")
        self.assertEqual(cpp.int_type(0, 2**32 - 1), "u32")
        self.assertEqual(cpp.int_type(0, 2**63 - 1), "u64")
        self.assertEqual(cpp.int_type(-128, 127), "i8")
        self.assertEqual(cpp.int_type(-129, 0), "i16")
        self.assertEqual(cpp.int_type(-1, 2**31), "i64")
        self.assertEqual(cpp.i64_literal(-(2**63)), "(-9223372036854775807 - 1)")
        self.assertEqual(cpp.i64_literal(-5), "-5")
        self.assertEqual(cpp.snake("ChatSend"), "chat_send")


FULL = HEADER + """
enum Kind : u16 { Aa = 1 Bb = 500 }
struct Inner { k: Kind }
struct Outer { inner: Inner list: array[3] of Inner }
""" + message("outer: Outer") + message("x: quantized[0, 1, 0.5]", name="Reply", id_=2,
                                        attributes="channel sequenced\n    from server\n"
                                                   "    rate 60/s burst 120") \
    + "event Paid = 7 { stream economy\n amount: int[1, 9] }\n"


class GenerationTest(unittest.TestCase):
    def generate(self, sources: dict[str, str]) -> tuple[str, str]:
        protocol = schema.loads(sources)
        names = sorted(sources)
        return (cpp.generate_header(protocol, "orion::protocol", names),
                cpp.generate_source(protocol, "orion::protocol", "x/protocol.hpp", names))

    def test_output_does_not_depend_on_file_order(self) -> None:
        split = FULL.split("struct Outer")
        a = {"a.schema": split[0], "b.schema": "struct Outer" + split[1]}
        b = {"z.schema": split[0], "b.schema": "struct Outer" + split[1]}
        header_a, source_a = self.generate(a)
        header_b, source_b = self.generate(b)
        strip = (lambda text: "\n".join(l for l in text.splitlines() if not l.startswith("//")))
        self.assertEqual(strip(header_a), strip(header_b))
        self.assertEqual(strip(source_a), strip(source_b))
        self.assertEqual(self.generate(a), self.generate(dict(reversed(list(a.items())))))

    def test_header_declares_everything_in_dependency_order(self) -> None:
        header, source = self.generate({"a.schema": FULL})
        self.assertIn("inline constexpr u32 kProtocolVersion = 1;", header)
        self.assertLess(header.index("struct Inner {"), header.index("struct Outer {"))
        self.assertIn("enum class Kind : u16 {", header)
        self.assertIn("using ClientMessage = std::variant<Hello>;", header)
        self.assertIn("using ServerMessage = std::variant<Reply>;", header)
        self.assertIn("using Event = std::variant<Paid>;", header)
        self.assertIn("static constexpr net::Channel kChannel = net::Channel::Sequenced;",
                      header)
        self.assertIn('static constexpr std::string_view kStream = "economy";', header)
        self.assertIn("decode_client_message(net::Channel channel", header)
        self.assertIn("Result<Event> decode_event(", source)
        # Hằng lượng tử hoá viết bằng literal hex: đúng giá trị Python đã kiểm.
        self.assertIn("const net::Quantization kQuantization0(0x0.0p+0, 0x1.0000000000000p+0, "
                      "0x1.0000000000000p-1);", source)
        self.assertIn('#include "x/protocol.hpp"', source)

    def test_fields_default_to_a_value_in_range(self) -> None:
        header, _ = self.generate({"a.schema": HEADER + "enum Kind : u8 { Bb = 7 Aa = 1 }\n"
                                   + message("a: int[5, 9]\n    b: quantized[10, 20, 0.5]\n"
                                             "    c: quantized[-1, 1, 0.5]\n    d: Kind\n"
                                             "    e: array[3] of bits(4)\n    f: string[8]")})
        self.assertIn("    u8 a = 5;  // int[5, 9]\n", header)
        self.assertIn("    f64 b = 0x1.4000000000000p+3;  // quantized[10, 20, 0.5]\n", header)
        self.assertIn("    f64 c = 0.0;  // quantized[-1, 1, 0.5]\n", header)
        self.assertIn("    Kind d = Kind::Bb;\n", header)
        self.assertIn("    BoundedArray<u8, 3> e{};  // array[3] of bits(4)\n", header)
        self.assertIn("    BoundedString<8> f{};\n", header)

    def test_lookup_tables_are_sorted_whatever_the_declaration_order(self) -> None:
        text = (HEADER + "enum Kind : u8 { Bb = 7 Aa = 1 }\n"
                + message("k: Kind", name="Late", id_=9) + message(name="Early", id_=2))
        _, source = self.generate({"a.schema": text})
        self.assertLess(source.index("{.id = Early::kId"), source.index("{.id = Late::kId"))
        self.assertLess(source.index("    Kind::Aa,"), source.index("    Kind::Bb,"))
        self.assertIn("static_assert(std::ranges::is_sorted(kClientMessageEntries, {}, kEntryId));",
                      source)
        # Bên đọc lấy giá trị enum từ bảng: không ép số nào từ mạng thành enum.
        self.assertNotIn("static_cast", source)

    def test_groups_that_are_empty_are_left_out(self) -> None:
        header, source = self.generate({"a.schema": HEADER + "event Aa = 1 { stream s }\n"})
        self.assertNotIn("ClientMessage", header)
        self.assertNotIn("net::Channel", header)
        self.assertNotIn("channels.hpp", header)
        self.assertIn("Result<usize> encode(const Aa& /*message*/", source)
        header, _ = self.generate({"a.schema": HEADER})
        self.assertNotIn("#include <span>", header)
        self.assertNotIn("error.hpp", header)


class CommandLineTest(unittest.TestCase):
    def run_main(self, *args: str) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = orion_codegen.main(list(args))
        return code, err.getvalue()

    def test_writes_header_and_source(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "p").mkdir()
            (root / "p" / "a.schema").write_text(FULL, encoding="utf-8")
            code, err = self.run_main(
                "--namespace", "orion::protocol", "--header", str(root / "out" / "x.hpp"),
                "--source", str(root / "out" / "x.cpp"), "--include", "x.hpp",
                "--root", str(root), str(root / "p" / "a.schema"))
            self.assertEqual((code, err), (0, ""))
            header = (root / "out" / "x.hpp").read_bytes().decode("utf-8")
            self.assertIn("//   p/a.schema", header)
            self.assertNotIn("\r\n", header)
            self.assertIn('#include "x.hpp"', (root / "out" / "x.cpp").read_text("utf-8"))

    def test_reports_schema_errors_with_location(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "a.schema").write_text("version 1\nmessage bad = 1 {}\n", encoding="utf-8")
            code, err = self.run_main(
                "--namespace", "orion::protocol", "--header", str(root / "x.hpp"),
                "--source", str(root / "x.cpp"), "--include", "x.hpp", "--root", str(root),
                str(root / "a.schema"))
            self.assertEqual(code, 1)
            self.assertTrue(err.startswith("a.schema:2:9: lỗi [schema]: "), err)
            self.assertFalse((root / "x.hpp").exists())

    def test_rejects_a_bad_namespace(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            orion_codegen.main(["--namespace", "Orion::Protocol", "--header", "x.hpp",
                                "--source", "x.cpp", "--include", "x.hpp", "a.schema"])


if __name__ == "__main__":
    unittest.main()
