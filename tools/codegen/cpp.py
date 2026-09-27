"""Sinh C++ từ mô hình schema (docs/formats/protocol.md, mục Mã sinh ra).

protocol.hpp khai kiểu, hằng và hàm; protocol.cpp cài đặt chúng bằng Encoder và Decoder của
game/shared/protocol/codec.hpp. Kết quả chỉ phụ thuộc mô hình (không phụ thuộc thứ tự tệp), nên
cùng schema luôn cho cùng byte.
"""

from __future__ import annotations

import dataclasses
from codegen import schema as s

CHANNEL_NAMES = {
    "unreliable": "Unreliable",
    "sequenced": "Sequenced",
    "reliable_ordered": "ReliableOrdered",
    "reliable_unordered": "ReliableUnordered",
}
SENDER_NAMES = {"client": "Client", "server": "Server"}


def _unsigned_type(bits: int) -> str:
    for width in (8, 16, 32, 64):
        if bits <= width:
            return f"u{width}"
    raise AssertionError(bits)


def int_type(lo: int, hi: int) -> str:
    """Kiểu C++ nhỏ nhất chứa [lo, hi] (protocol.md, bảng Kiểu)."""
    if lo >= 0:
        return _unsigned_type(hi.bit_length())
    for width in (8, 16, 32, 64):
        if -(2 ** (width - 1)) <= lo and hi < 2 ** (width - 1):
            return f"i{width}"
    raise AssertionError((lo, hi))


def i64_literal(value: int) -> str:
    # -2^63 không viết được thành một literal: literal dương 2^63 không vừa i64.
    return "(-9223372036854775807 - 1)" if value == s.I64_MIN else str(value)


def snake(name: str) -> str:
    """CamelCase sang snake_case cho tên hàm đọc message: ChatSend -> chat_send."""
    out = []
    for i, char in enumerate(name):
        if char.isupper() and i > 0:
            out.append("_")
        out.append(char.lower())
    return "".join(out)


@dataclasses.dataclass
class _Context:
    protocol: s.Protocol
    quantizations: dict[s.QuantizedType, str]

    def type(self, type_: s.Type) -> str:
        match type_:
            case s.BoolType():
                return "bool"
            case s.BitsType(count=count):
                return _unsigned_type(count)
            case s.IntType(min=lo, max=hi):
                return int_type(lo, hi)
            case s.QuantizedType():
                return "f64"
            case s.BytesType(max=size):
                return f"BoundedBytes<{size}>"
            case s.StringType(max=size):
                return f"BoundedString<{size}>"
            case s.TickType():
                return "Tick"
            case s.ShortTickType():
                return "ShortTick"
            case s.ReplicatedIdType():
                return "ReplicatedId"
            case s.ArrayType(max=size, element=element):
                return f"BoundedArray<{self.type(element)}, {size}>"
            case s.NamedType(name=name):
                return name
        raise AssertionError(type_)

    def initializer(self, type_: s.Type) -> str:
        match type_:
            case s.BoolType():
                return " = false"
            case s.BitsType():
                return " = 0"
            case s.IntType(min=lo, max=hi):
                return f" = {i64_literal(0 if lo <= 0 <= hi else lo)}"
            case s.QuantizedType(min=lo, max=hi):
                # Như int: 0, hay min khi 0 ngoài khoảng (min là chỉ số 0 nên khứ hồi đúng).
                return " = 0.0" if lo <= 0 <= hi else f" = {_hex_float(lo)}"
            case s.NamedType(name=name):
                decl = self.protocol.lookup(name)
                if isinstance(decl, s.Enum):
                    return f" = {name}::{decl.values[0].name}"
        return ""

    def write(self, type_: s.Type, value: str, depth: int) -> list[str]:
        """Các lệnh ghi một giá trị kiểu type_ vào `encoder`."""
        match type_:
            case s.BoolType():
                return [f"encoder.write_bool({value});"]
            case s.BitsType(count=count):
                return [f"encoder.write_bits({value}, {count});"]
            case s.IntType(min=lo, max=hi):
                return [f"encoder.write_int({value}, {i64_literal(lo)}, {i64_literal(hi)});"]
            case s.QuantizedType():
                return [f"encoder.write_quantized({value}, {self.quantizations[type_]});"]
            case s.BytesType():
                return [f"encoder.write_bytes({value});"]
            case s.StringType():
                return [f"encoder.write_string({value});"]
            case s.TickType():
                return [f"encoder.write_tick({value});"]
            case s.ShortTickType():
                return [f"encoder.write_short_tick({value});"]
            case s.ReplicatedIdType():
                return [f"encoder.write_replicated_id({value});"]
            case s.ArrayType(max=size, element=element):
                item = f"item{depth}"
                body = self.write(element, item, depth + 1)
                return [f"encoder.write_length({value}.size(), {size});",
                        f"for (const auto& {item} : {value}) {{",
                        *(f"    {line}" for line in body), "}"]
            case s.NamedType():
                return [f"write(encoder, {value});"]
        raise AssertionError(type_)

    def read(self, type_: s.Type, value: str, depth: int) -> list[str]:
        """Các lệnh đọc một giá trị kiểu type_ từ `decoder` vào lvalue `value`."""
        match type_:
            case s.BoolType():
                return [f"decoder.read_bool({value});"]
            case s.BitsType(count=count):
                return [f"decoder.read_bits({value}, {count});"]
            case s.IntType(min=lo, max=hi):
                return [f"decoder.read_int({value}, {i64_literal(lo)}, {i64_literal(hi)});"]
            case s.QuantizedType():
                return [f"decoder.read_quantized({value}, {self.quantizations[type_]});"]
            case s.BytesType():
                return [f"decoder.read_bytes({value});"]
            case s.StringType():
                return [f"decoder.read_string({value});"]
            case s.TickType():
                return [f"decoder.read_tick({value});"]
            case s.ShortTickType():
                return [f"decoder.read_short_tick({value});"]
            case s.ReplicatedIdType():
                return [f"decoder.read_replicated_id({value});"]
            case s.ArrayType(max=size, element=element):
                item = f"item{depth}"
                body = self.read(element, item, depth + 1)
                return [f"{value}.resize(decoder.read_length({size}));",
                        f"for (auto& {item} : {value}.view()) {{",
                        *(f"    {line}" for line in body), "}"]
            case s.NamedType():
                return [f"read(decoder, {value});"]
        raise AssertionError(type_)


def schema_text(type_: s.Type) -> str:
    """Kiểu như viết trong schema, cho chú thích của code sinh ra."""
    match type_:
        case s.BitsType(count=count):
            return f"bits({count})"
        case s.IntType(min=lo, max=hi):
            return f"int[{lo}, {hi}]"
        case s.QuantizedType(text=text):
            return f"quantized[{', '.join(text)}]"
        case s.BytesType(max=size):
            return f"bytes[{size}]"
        case s.StringType(max=size):
            return f"string[{size}]"
        case s.ArrayType(max=size, element=element):
            return f"array[{size}] of {schema_text(element)}"
        case s.NamedType(name=name):
            return name
    return {s.BoolType: "bool", s.TickType: "tick", s.ShortTickType: "short_tick",
            s.ReplicatedIdType: "replicated_id"}[type(type_)]


def _field_comment(type_: s.Type) -> str:
    # Kiểu C++ của bits, int, quantized không nói khoảng đã khai: ghi lại kiểu schema.
    narrowed = (s.BitsType, s.IntType, s.QuantizedType)
    if any(isinstance(t, narrowed) for t in s.walk(type_)):
        return f"  // {schema_text(type_)}"
    return ""


def _all_types(protocol: s.Protocol) -> list[s.Type]:
    decls = (*protocol.structs, *protocol.messages, *protocol.events)
    return [t for decl in decls for field in decl.fields for t in s.walk(field.type)]


def _struct_order(protocol: s.Protocol) -> list[s.Struct]:
    """Struct theo thứ tự phụ thuộc (struct được dùng đứng trước), hoà thì theo tên."""
    structs = {st.name: st for st in protocol.structs}
    ordered: list[s.Struct] = []
    placed: set[str] = set()

    def place(struct: s.Struct) -> None:
        if struct.name in placed:
            return
        for field in struct.fields:
            for t in s.walk(field.type):
                if isinstance(t, s.NamedType) and t.name in structs:
                    place(structs[t.name])
        placed.add(struct.name)
        ordered.append(struct)

    for name in sorted(structs):
        place(structs[name])
    return ordered


def _hex_float(value: float) -> str:
    # Literal hex của C++17 cho đúng giá trị double mà Python đã dùng khi kiểm schema.
    return value.hex()


def _header_includes(protocol: s.Protocol, uses_codec: bool) -> list[str]:
    has_functions = bool(protocol.messages or protocol.events)
    project = ['"engine/core/types.hpp"']
    system: list[str] = []
    if has_functions:
        project.append('"engine/core/error.hpp"')
        system += ["<cstddef>", "<span>", "<variant>"]
    if protocol.messages:
        project += ['"engine/net/channels.hpp"', '"engine/net/rate_limit.hpp"']
    if uses_codec:
        project.append('"game/shared/protocol/codec.hpp"')
    if protocol.events:
        system.append("<string_view>")
    lines = [f"#include {p}" for p in sorted(project)]
    if system:
        lines += ["", *(f"#include {p}" for p in sorted(system))]
    return lines


def _uses_codec_types(protocol: s.Protocol) -> bool:
    codec = (s.BytesType, s.StringType, s.TickType, s.ShortTickType, s.ReplicatedIdType,
             s.ArrayType)
    return bool(protocol.messages) or any(isinstance(t, codec) for t in _all_types(protocol))


def _banner(sources: list[str]) -> list[str]:
    return ["// Sinh bởi tools/codegen/orion_codegen.py từ:",
            *(f"//   {path}" for path in sorted(sources)),
            "// Không sửa tay (ADR 0004; docs/formats/protocol.md, mục Mã sinh ra)."]


def _struct_lines(ctx: _Context, name: str, statics: list[str],
                  fields: tuple[s.Field, ...]) -> list[str]:
    lines = [f"struct {name} {{", *(f"    {line}" for line in statics)]
    if statics and fields:
        lines.append("")
    for field in fields:
        lines.append(f"    {ctx.type(field.type)} {field.name}{ctx.initializer(field.type)};"
                     f"{_field_comment(field.type)}")
    lines += ["", f"    friend bool operator==(const {name}&, const {name}&) = default;", "};", ""]
    return lines


def generate_header(protocol: s.Protocol, namespace: str, sources: list[str]) -> str:
    ctx = _Context(protocol, {})
    lines = [*_banner(sources), "#pragma once", "",
             *_header_includes(protocol, _uses_codec_types(protocol)), "",
             f"namespace {namespace} {{", "",
             f"inline constexpr u32 kProtocolVersion = {protocol.version};", ""]
    for enum in sorted(protocol.enums, key=lambda e: e.name):
        lines.append(f"enum class {enum.name} : {enum.underlying} {{")
        lines += [f"    {v.name} = {v.value}," for v in enum.values]
        lines += ["};", ""]
    for struct in _struct_order(protocol):
        lines += _struct_lines(ctx, struct.name, [], struct.fields)
    for message in protocol.messages:
        statics = [
            f"static constexpr u32 kId = {message.id};",
            f"static constexpr net::Channel kChannel = net::Channel::"
            f"{CHANNEL_NAMES[message.channel]};",
            f"static constexpr Sender kSender = Sender::{SENDER_NAMES[message.sender]};",
            f"static constexpr net::RateLimit kRateLimit = net::RateLimit::per_second("
            f"{message.rate}, {message.burst});",
            f"static constexpr usize kMaxEncodedSize = {protocol.max_encoded_size(message)};",
        ]
        lines += _struct_lines(ctx, message.name, statics, message.fields)
    for event in protocol.events:
        statics = [
            f"static constexpr u32 kId = {event.id};",
            f'static constexpr std::string_view kStream = "{event.stream}";',
            f"static constexpr usize kMaxEncodedSize = {protocol.max_encoded_size(event)};",
        ]
        lines += _struct_lines(ctx, event.name, statics, event.fields)
    lines += _variants_and_functions(protocol)
    lines += [f"}}  // namespace {namespace}", ""]
    return "\n".join(lines)


def _variants_and_functions(protocol: s.Protocol) -> list[str]:
    lines: list[str] = []
    groups = [("ClientMessage", [m for m in protocol.messages if m.sender == "client"]),
              ("ServerMessage", [m for m in protocol.messages if m.sender == "server"]),
              ("Event", list(protocol.events))]
    for alias, members in groups:
        if members:
            lines.append(f"using {alias} = std::variant<{', '.join(m.name for m in members)}>;")
    if protocol.messages:
        size = max(protocol.max_encoded_size(m) for m in protocol.messages)
        lines.append(f"inline constexpr usize kMaxMessageSize = {size};")
    if protocol.events:
        size = max(protocol.max_encoded_size(e) for e in protocol.events)
        lines.append(f"inline constexpr usize kMaxEventSize = {size};")
    if lines:
        lines.append("")
    if protocol.messages or protocol.events:
        lines += ["// Ghi vào `out`, trả số byte. Lỗi: InvalidArgument khi một trường ngoài khoảng",
                  "// đã khai, ResourceExhausted khi `out` nhỏ hơn cỡ cần."]
    for decl in (*protocol.messages, *protocol.events):
        lines.append(f"[[nodiscard]] Result<usize> encode(const {decl.name}& message, "
                     f"std::span<std::byte> out) noexcept;")
    if protocol.messages or protocol.events:
        lines += ["", "// Hàm toàn phần: kiểm hết rồi mới trả (protocol.md, mục Tin nhắn)."]
    for alias, members in groups[:2]:
        if members:
            lines.append(f"[[nodiscard]] Result<{alias}> decode_{snake(alias)}("
                         f"net::Channel channel, std::span<const std::byte> bytes) noexcept;")
    if protocol.events:
        lines.append("[[nodiscard]] Result<Event> decode_event(std::span<const std::byte> bytes) "
                     "noexcept;")
    if protocol.messages or protocol.events:
        lines.append("")
    return lines


def _source_includes(protocol: s.Protocol, header_include: str, has_quantized: bool) -> list[str]:
    lines = [f'#include "{header_include}"']
    if not (protocol.messages or protocol.events):
        return lines
    project = ['"engine/core/error.hpp"', '"engine/core/types.hpp"',
               '"game/shared/protocol/codec.hpp"']
    if has_quantized:
        project.append('"engine/net/bitstream.hpp"')
    if protocol.messages:
        project.append('"engine/net/channels.hpp"')
    system = ["<algorithm>", "<array>", "<cstddef>", "<expected>", "<span>"]
    if protocol.enums:
        system.append("<utility>")
    return [*lines, "", *(f"#include {p}" for p in sorted(project)), "",
            *(f"#include {p}" for p in sorted(system))]


def _enum_codec(enum: s.Enum) -> list[str]:
    # Bảng giá trị đã khai theo số tăng dần: kiểm bằng tìm nhị phân, và bên đọc lấy giá trị từ bảng
    # nên không bao giờ ép một số chưa kiểm thành enum. Hàm dài cố định dù enum nhiều giá trị.
    table = f"k{enum.name}Values"
    ordered = sorted(enum.values, key=lambda v: v.value)
    lo, hi = enum.min_value, enum.max_value
    return [
        f"constexpr std::array<{enum.name}, {len(ordered)}> {table} = {{",
        *(f"    {enum.name}::{v.name}," for v in ordered),
        "};",
        f"static_assert(std::ranges::is_sorted({table}));",
        "",
        f"void write(Encoder& encoder, const {enum.name} value) noexcept {{",
        f"    if (!std::ranges::binary_search({table}, value)) {{",
        "        encoder.reject();",
        "        return;",
        "    }",
        f"    encoder.write_int(std::to_underlying(value), {lo}, {hi});",
        "}",
        "",
        f"void read(Decoder& decoder, {enum.name}& value) noexcept {{",
        f"    {enum.underlying} raw = 0;",
        f"    decoder.read_int(raw, {lo}, {hi});",
        f"    const {enum.name}* const found = find_sorted({table}, raw, kUnderlying);",
        "    if (found == nullptr) {",
        "        decoder.reject();",
        "        return;",
        "    }",
        "    value = *found;",
        "}",
        "",
    ]


def _struct_codec(ctx: _Context, struct: s.Struct) -> list[str]:
    lines = [f"void write(Encoder& encoder, const {struct.name}& value) noexcept {{"]
    for field in struct.fields:
        lines += [f"    {line}" for line in ctx.write(field.type, f"value.{field.name}", 0)]
    lines += ["}", "", f"void read(Decoder& decoder, {struct.name}& value) noexcept {{"]
    for field in struct.fields:
        lines += [f"    {line}" for line in ctx.read(field.type, f"value.{field.name}", 0)]
    lines += ["}", ""]
    return lines


def _reader(ctx: _Context, decl: s.Message | s.EventDecl) -> list[str]:
    if not decl.fields:
        return [f"void read(Decoder& /*decoder*/, {decl.name}& /*message*/) noexcept {{}}", ""]
    lines = [f"void read(Decoder& decoder, {decl.name}& message) noexcept {{"]
    for field in decl.fields:
        lines += [f"    {line}" for line in ctx.read(field.type, f"message.{field.name}", 0)]
    lines += ["}", ""]
    return lines


def _dispatch_tables(protocol: s.Protocol) -> list[str]:
    """Bảng id → hàm đọc của từng nhóm, theo id tăng dần: hàm giải mã tìm nhị phân trong bảng, nên
    dài cố định dù protocol có bao nhiêu message (CLAUDE.md X.2)."""
    lines = ["// Đọc thẳng vào phương án của variant, không qua bản sao tạm.",
             "template <class Decl, class Group>",
             "void read_into(Decoder& decoder, Group& group) noexcept {",
             "    read(decoder, group.template emplace<Decl>());",
             "}",
             ""]
    lines += ["constexpr auto kEntryId = [](const auto& entry) noexcept { return entry.id; };", ""]
    if protocol.messages:
        lines += ["template <class Group>",
                  "struct MessageEntry {",
                  "    u32 id;",
                  "    net::Channel channel;",
                  "    void (*read)(Decoder& decoder, Group& group) noexcept;",
                  "};",
                  ""]
    for alias, table, sender in (("ClientMessage", "kClientMessageEntries", "client"),
                                 ("ServerMessage", "kServerMessageEntries", "server")):
        members = [m for m in protocol.messages if m.sender == sender]
        if members:
            lines += [f"constexpr std::array<MessageEntry<{alias}>, {len(members)}> {table} = {{{{",
                      *(f"    {{.id = {m.name}::kId, .channel = {m.name}::kChannel, "
                        f".read = &read_into<{m.name}, {alias}>}}," for m in members),
                      "}};",
                      f"static_assert(std::ranges::is_sorted({table}, {{}}, kEntryId));",
                      ""]
    if protocol.events:
        lines += ["struct EventEntry {",
                  "    u32 id;",
                  "    void (*read)(Decoder& decoder, Event& event) noexcept;",
                  "};",
                  "",
                  f"constexpr std::array<EventEntry, {len(protocol.events)}> kEventEntries = {{{{",
                  *(f"    {{.id = {e.name}::kId, .read = &read_into<{e.name}, Event>}},"
                    for e in protocol.events),
                  "}};",
                  "static_assert(std::ranges::is_sorted(kEventEntries, {}, kEntryId));",
                  ""]
    return lines


def _internals(ctx: _Context, quantized: list[s.QuantizedType]) -> list[str]:
    """Nội dung namespace ẩn của protocol.cpp: hằng, bảng và hàm đọc ghi từng kiểu."""
    protocol = ctx.protocol
    lines: list[str] = []
    if protocol.messages:
        lines.append(f"constexpr i64 kMessageIdMax = {protocol.max_message_id};")
    if protocol.events:
        lines.append(f"constexpr i64 kEventIdMax = {protocol.max_event_id};")
    lines.append("")
    for q in quantized:
        lines += [f"// {schema_text(q)}",
                  f"const net::Quantization {ctx.quantizations[q]}({_hex_float(q.min)}, "
                  f"{_hex_float(q.max)}, {_hex_float(q.step)});"]
    if quantized:
        lines.append("")
    # Tìm qua span: iterator của nó là class trên mọi thư viện chuẩn, nên cùng một code sạch với
    # clang-tidy ở cả libstdc++ (iterator của std::array là con trỏ) lẫn MSVC.
    lines += ["// Phần tử có khoá `key` trong `table` xếp tăng dần theo `key_of`, hay nullptr.",
              "template <class T, usize N, class Key, class KeyOf>",
              "[[nodiscard]] const T* find_sorted(const std::array<T, N>& table, const Key key,",
              "                                   const KeyOf key_of) noexcept {",
              "    const std::span<const T> view(table);",
              "    const auto found = std::ranges::lower_bound(view, key, {}, key_of);",
              "    return (found != view.end() && key_of(*found) == key) ? &*found : nullptr;",
              "}",
              ""]
    if protocol.enums:
        lines += ["constexpr auto kUnderlying = [](const auto value) noexcept {",
                  "    return std::to_underlying(value);",
                  "};",
                  ""]
    structs = _struct_order(protocol)
    if structs:
        lines.append("// Khai trước: struct dùng struct khác.")
        for struct in structs:
            lines += [f"void write(Encoder& encoder, const {struct.name}& value) noexcept;",
                      f"void read(Decoder& decoder, {struct.name}& value) noexcept;"]
        lines.append("")
    for enum in sorted(protocol.enums, key=lambda e: e.name):
        lines += _enum_codec(enum)
    for struct in structs:
        lines += _struct_codec(ctx, struct)
    for decl in (*protocol.messages, *protocol.events):
        lines += _reader(ctx, decl)
    return lines + _dispatch_tables(protocol)


def _encoder(ctx: _Context, decl: s.Message | s.EventDecl, id_max: str) -> list[str]:
    # Không có trường thì không dùng tham số: bỏ tên để không bị -Wunused-parameter.
    param = "message" if decl.fields else "/*message*/"
    lines = [f"Result<usize> encode(const {decl.name}& {param}, const std::span<std::byte> out) "
             "noexcept {",
             "    Encoder encoder(out);",
             f"    encoder.write_int({decl.name}::kId, 0, {id_max});"]
    for field in decl.fields:
        lines += [f"    {line}" for line in ctx.write(field.type, f"message.{field.name}", 0)]
    lines += ["    return encoder.finish();", "}", ""]
    return lines


def _decoder(alias: str, table: str, id_max: str, with_channel: bool) -> list[str]:
    params = "const net::Channel channel, " if with_channel else ""
    match = "entry != nullptr" + (" && entry->channel == channel" if with_channel else "")
    return [f"Result<{alias}> decode_{snake(alias)}({params}"
            "const std::span<const std::byte> bytes) noexcept {",
            "    Decoder decoder(bytes);",
            "    u32 id = 0;",
            f"    decoder.read_int(id, 0, {id_max});",
            f"    {alias} message;",
            f"    const auto* const entry = find_sorted({table}, id, kEntryId);",
            f"    if ({match}) {{",
            "        entry->read(decoder, message);",
            "    } else {",
            "        decoder.reject();",
            "    }",
            "    if (const Result<void> finished = decoder.finish(); !finished) {",
            "        return std::unexpected(finished.error());",
            "    }",
            "    return message;",
            "}",
            ""]


def generate_source(protocol: s.Protocol, namespace: str, header_include: str,
                    sources: list[str]) -> str:
    quantized = sorted({t for t in _all_types(protocol) if isinstance(t, s.QuantizedType)},
                       key=lambda q: (q.min, q.max, q.step))
    ctx = _Context(protocol, {q: f"kQuantization{i}" for i, q in enumerate(quantized)})
    lines = [*_banner(sources),
             *_source_includes(protocol, header_include, bool(quantized)), "",
             f"namespace {namespace} {{", ""]
    if protocol.messages or protocol.events:
        lines += ["namespace {", "", *_internals(ctx, quantized), "}  // namespace", ""]
    for message in protocol.messages:
        lines += _encoder(ctx, message, "kMessageIdMax")
    for event in protocol.events:
        lines += _encoder(ctx, event, "kEventIdMax")
    if any(m.sender == "client" for m in protocol.messages):
        lines += _decoder("ClientMessage", "kClientMessageEntries", "kMessageIdMax", True)
    if any(m.sender == "server" for m in protocol.messages):
        lines += _decoder("ServerMessage", "kServerMessageEntries", "kMessageIdMax", True)
    if protocol.events:
        lines += _decoder("Event", "kEventEntries", "kEventIdMax", False)
    lines += [f"}}  // namespace {namespace}", ""]
    return "\n".join(lines)
