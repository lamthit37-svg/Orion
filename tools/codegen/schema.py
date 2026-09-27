"""Ngôn ngữ schema của protocol (docs/formats/protocol.md, mục Ngôn ngữ schema): đọc tệp, kiểm mọi
luật của mục đó, và dựng mô hình cho bộ sinh C++ (cpp.py).

Lỗi đầu tiên dừng việc đọc và được báo theo dạng `tệp:dòng:cột: lỗi [schema]: thông điệp`, như các
cổng kiểm. Chỉ dùng thư viện chuẩn của Python 3.13 (hiến pháp V.5).
"""

from __future__ import annotations

import dataclasses
import math
import pathlib
import re
from collections.abc import Iterable, Sequence

# Luật đặt tên (protocol.md, mục Từ vựng).
TYPE_NAME = re.compile(r"[A-Z][a-z0-9]+(?:[A-Z][a-z0-9]+)*")
FIELD_NAME = re.compile(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*")

# Từ khoá của C++23, các token thay thế, các định danh có nghĩa riêng (final, override, import,
# module) và các macro đối tượng của thư viện C chuẩn: không dùng làm tên trường vì code sinh ra đặt
# tên trường thẳng vào C++.
CPP_RESERVED = frozenset("""
    alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
    char32_t class compl concept const consteval constexpr constinit const_cast continue co_await
    co_return co_yield decltype default delete do double dynamic_cast else enum explicit export
    extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq
    nullptr operator or or_eq private protected public register reinterpret_cast requires return
    short signed sizeof static static_assert static_cast struct switch template this thread_local
    throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while
    xor xor_eq final override import module errno stdin stdout stderr
""".split())

# Kiểu số của engine/core/types.hpp. Struct sinh ra dùng chúng không kèm namespace, nên một trường
# cùng tên đổi nghĩa của tên đó trong struct: GCC báo lỗi (-Wchanges-meaning), clang im lặng
# nhận một chương trình sai ([basic.scope.class]).
SCALAR_TYPES = frozenset("i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 usize isize".split())

# Tên kiểu mà code hỗ trợ và code sinh ra đã dùng trong namespace của protocol.
RESERVED_TYPE_NAMES = frozenset({
    "Tick", "ShortTick", "ReplicatedId", "BoundedBytes", "BoundedString", "BoundedArray",
    "Encoder", "Decoder", "Sender", "ClientMessage", "ServerMessage", "Event", "MessageEntry",
    "EventEntry", "Result", "Error", "ErrorCode",
})

CHANNELS = ("unreliable", "sequenced", "reliable_ordered", "reliable_unordered")
SENDERS = ("client", "server")
UNDERLYING_BITS = {"u8": 8, "u16": 16, "u32": 32}

MAX_ID = 65_535
MAX_VERSION = 2**32 - 1
MAX_LENGTH = 65_535
MAX_RATE = 10**9
MAX_BURST = 10**6
MAX_QUANTIZATION_INDEX = 2**32
I64_MIN = -(2**63)
I64_MAX = 2**63 - 1
# Cỡ lớn nhất trên dây theo kênh (protocol.md, mục Giới hạn cỡ; channels.md).
UNRELIABLE_LIMIT = 1_150
RELIABLE_LIMIT = 32_768
EVENT_LIMIT = 32_768

_TOKEN = re.compile(r"""
    (?P<space>[ \t\r]+)
  | (?P<newline>\n)
  | (?P<comment>\#[^\n]*)
  | (?P<number>-?[0-9]+(?:\.[0-9]+)?)
  | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<punct>[{}\[\]():,=/])
""", re.VERBOSE)


@dataclasses.dataclass(frozen=True)
class Location:
    path: str
    line: int
    column: int


class SchemaError(Exception):
    """Lỗi của schema, kèm vị trí trong tệp."""

    def __init__(self, location: Location, message: str) -> None:
        super().__init__(message)
        self.location = location
        self.message = message

    def format(self) -> str:
        loc = self.location
        return f"{loc.path}:{loc.line}:{loc.column}: lỗi [schema]: {self.message}"


@dataclasses.dataclass(frozen=True)
class Token:
    kind: str  # number | name | punct | end
    text: str
    location: Location


def tokenize(text: str, path: str) -> list[Token]:
    tokens: list[Token] = []
    line, line_start, pos = 1, 0, 0
    while pos < len(text):
        match = _TOKEN.match(text, pos)
        location = Location(path, line, pos - line_start + 1)
        if match is None:
            raise SchemaError(location, f"ký tự lạ {text[pos]!r}")
        kind = match.lastgroup or ""
        if kind == "newline":
            line, line_start = line + 1, match.end()
        elif kind not in ("space", "comment"):
            tokens.append(Token(kind, match.group(), location))
        pos = match.end()
    tokens.append(Token("end", "", Location(path, line, pos - line_start + 1)))
    return tokens


# Mô hình kiểu. NamedType trỏ tới enum hay struct sau khi resolve.


@dataclasses.dataclass(frozen=True)
class BoolType:
    pass


@dataclasses.dataclass(frozen=True)
class BitsType:
    count: int


@dataclasses.dataclass(frozen=True)
class IntType:
    min: int
    max: int


@dataclasses.dataclass(frozen=True)
class QuantizedType:
    min: float
    max: float
    step: float
    # Chữ số như viết trong schema, để chú thích code sinh ra dễ đọc.
    text: tuple[str, str, str]

    @property
    def max_index(self) -> int:
        # Cùng phép IEEE với net::Quantization: chia làm tròn đúng rồi ceil.
        return math.ceil((self.max - self.min) / self.step)


@dataclasses.dataclass(frozen=True)
class BytesType:
    max: int


@dataclasses.dataclass(frozen=True)
class StringType:
    max: int


@dataclasses.dataclass(frozen=True)
class TickType:
    pass


@dataclasses.dataclass(frozen=True)
class ShortTickType:
    pass


@dataclasses.dataclass(frozen=True)
class ReplicatedIdType:
    pass


@dataclasses.dataclass(frozen=True)
class ArrayType:
    max: int
    element: Type


@dataclasses.dataclass(frozen=True)
class NamedType:
    name: str
    location: Location


Type = (BoolType | BitsType | IntType | QuantizedType | BytesType | StringType | TickType
        | ShortTickType | ReplicatedIdType | ArrayType | NamedType)


@dataclasses.dataclass(frozen=True)
class Field:
    name: str
    type: Type
    location: Location


@dataclasses.dataclass(frozen=True)
class EnumValue:
    name: str
    value: int
    location: Location


@dataclasses.dataclass(frozen=True)
class Enum:
    name: str
    underlying: str
    values: tuple[EnumValue, ...]
    location: Location

    @property
    def min_value(self) -> int:
        return min(v.value for v in self.values)

    @property
    def max_value(self) -> int:
        return max(v.value for v in self.values)


@dataclasses.dataclass(frozen=True)
class Struct:
    name: str
    fields: tuple[Field, ...]
    location: Location


@dataclasses.dataclass(frozen=True)
class Message:
    name: str
    id: int
    channel: str
    sender: str
    rate: int
    burst: int
    fields: tuple[Field, ...]
    location: Location


@dataclasses.dataclass(frozen=True)
class EventDecl:
    name: str
    id: int
    stream: str
    fields: tuple[Field, ...]
    location: Location


@dataclasses.dataclass(frozen=True)
class Protocol:
    version: int
    enums: tuple[Enum, ...]
    structs: tuple[Struct, ...]
    messages: tuple[Message, ...]
    events: tuple[EventDecl, ...]

    def lookup(self, name: str) -> Enum | Struct:
        for decl in (*self.enums, *self.structs):
            if decl.name == name:
                return decl
        raise KeyError(name)

    @property
    def max_message_id(self) -> int:
        return max((m.id for m in self.messages), default=0)

    @property
    def max_event_id(self) -> int:
        return max((e.id for e in self.events), default=0)

    def max_bits(self, type_: Type) -> int:
        """Số bit lớn nhất của một trường kiểu type_ trên dây (protocol.md, mục Kiểu)."""
        match type_:
            case BoolType():
                return 1
            case BitsType(count=count):
                return count
            case IntType(min=lo, max=hi):
                return (hi - lo).bit_length()
            case QuantizedType():
                return type_.max_index.bit_length()
            case BytesType(max=size) | StringType(max=size):
                return size.bit_length() + 8 * size
            case TickType():
                return 64
            case ShortTickType():
                return 16
            case ReplicatedIdType():
                return 32
            case ArrayType(max=size, element=element):
                return size.bit_length() + size * self.max_bits(element)
            case NamedType(name=name):
                decl = self.lookup(name)
                if isinstance(decl, Enum):
                    return (decl.max_value - decl.min_value).bit_length()
                return sum(self.max_bits(f.type) for f in decl.fields)
        raise AssertionError(type_)

    def max_encoded_size(self, decl: Message | EventDecl) -> int:
        """Cỡ lớn nhất tính bằng byte: trường id, các trường, đệm tới hết byte."""
        id_bits = (self.max_message_id if isinstance(decl, Message) else self.max_event_id)
        bits = id_bits.bit_length() + sum(self.max_bits(f.type) for f in decl.fields)
        return (bits + 7) // 8


class _Parser:
    def __init__(self, tokens: Sequence[Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    def peek(self, offset: int = 0) -> Token:
        return self._tokens[min(self._pos + offset, len(self._tokens) - 1)]

    def next(self) -> Token:
        token = self.peek()
        self._pos += 1
        return token

    def expect(self, text: str) -> Token:
        token = self.next()
        if token.text != text or token.kind not in ("punct", "name"):
            raise SchemaError(token.location, f"cần '{text}', gặp {_describe(token)}")
        return token

    def name(self, what: str) -> Token:
        token = self.next()
        if token.kind != "name":
            raise SchemaError(token.location, f"cần {what}, gặp {_describe(token)}")
        return token

    def integer(self, what: str) -> tuple[int, Location]:
        token = self.next()
        if token.kind != "number" or "." in token.text:
            raise SchemaError(token.location, f"cần {what} là số nguyên, gặp {_describe(token)}")
        return int(token.text), token.location

    def real(self, what: str) -> tuple[float, str, Location]:
        token = self.next()
        if token.kind != "number":
            raise SchemaError(token.location, f"cần {what} là số, gặp {_describe(token)}")
        return float(token.text), token.text, token.location


def _describe(token: Token) -> str:
    return "hết tệp" if token.kind == "end" else f"'{token.text}'"


@dataclasses.dataclass
class _Declarations:
    versions: list[tuple[int, Location]] = dataclasses.field(default_factory=list)
    enums: list[Enum] = dataclasses.field(default_factory=list)
    structs: list[Struct] = dataclasses.field(default_factory=list)
    messages: list[Message] = dataclasses.field(default_factory=list)
    events: list[EventDecl] = dataclasses.field(default_factory=list)


def _parse_file(text: str, path: str, out: _Declarations) -> None:
    parser = _Parser(tokenize(text, path))
    while parser.peek().kind != "end":
        keyword = parser.next()
        match keyword.text if keyword.kind == "name" else "":
            case "version":
                value, location = parser.integer("version")
                out.versions.append((value, location))
            case "enum":
                out.enums.append(_parse_enum(parser, keyword.location))
            case "struct":
                out.structs.append(_parse_struct(parser, keyword.location))
            case "message":
                out.messages.append(_parse_message(parser, keyword.location))
            case "event":
                out.events.append(_parse_event(parser, keyword.location))
            case _:
                raise SchemaError(keyword.location,
                                  f"cần version, enum, struct, message hay event, gặp "
                                  f"{_describe(keyword)}")


def _type_name(parser: _Parser) -> Token:
    token = parser.name("tên kiểu")
    if not TYPE_NAME.fullmatch(token.text):
        raise SchemaError(token.location,
                          f"tên kiểu '{token.text}' phải viết CamelCase, mỗi từ có chữ thường")
    if token.text in RESERVED_TYPE_NAMES:
        raise SchemaError(token.location, f"tên '{token.text}' đã được code hỗ trợ dùng")
    return token


def _parse_enum(parser: _Parser, location: Location) -> Enum:
    name = _type_name(parser)
    parser.expect(":")
    underlying = parser.name("kiểu nền")
    if underlying.text not in UNDERLYING_BITS:
        raise SchemaError(underlying.location,
                          f"kiểu nền phải là u8, u16 hay u32, gặp '{underlying.text}'")
    parser.expect("{")
    values: list[EnumValue] = []
    while parser.peek().text != "}" or parser.peek().kind != "punct":
        value_name = parser.name("tên giá trị enum")
        if not TYPE_NAME.fullmatch(value_name.text):
            raise SchemaError(value_name.location,
                              f"giá trị enum '{value_name.text}' phải viết CamelCase")
        parser.expect("=")
        number, number_location = parser.integer("số của giá trị enum")
        if number < 0 or number >= 2 ** UNDERLYING_BITS[underlying.text]:
            raise SchemaError(number_location,
                              f"{number} không vừa kiểu nền {underlying.text}")
        values.append(EnumValue(value_name.text, number, value_name.location))
    parser.expect("}")
    if not values:
        raise SchemaError(name.location, f"enum '{name.text}' không có giá trị nào")
    _check_unique((v.name for v in values), (v.location for v in values), "giá trị enum")
    _check_unique((str(v.value) for v in values), (v.location for v in values),
                  "số của giá trị enum")
    return Enum(name.text, underlying.text, tuple(values), location)


def _parse_fields(parser: _Parser) -> tuple[Field, ...]:
    fields: list[Field] = []
    while parser.peek().text != "}" or parser.peek().kind != "punct":
        name = parser.name("tên trường")
        if not FIELD_NAME.fullmatch(name.text):
            raise SchemaError(name.location, f"tên trường '{name.text}' phải viết snake_case")
        if name.text in CPP_RESERVED:
            raise SchemaError(name.location, f"tên trường '{name.text}' là từ khoá của C++")
        if name.text in SCALAR_TYPES:
            raise SchemaError(name.location,
                              f"tên trường '{name.text}' trùng kiểu số của engine/core/types.hpp")
        parser.expect(":")
        fields.append(Field(name.text, _parse_type(parser), name.location))
    parser.expect("}")
    _check_unique((f.name for f in fields), (f.location for f in fields), "trường")
    return tuple(fields)


def _parse_type(parser: _Parser) -> Type:
    token = parser.name("kiểu")
    match token.text:
        case "bool":
            return BoolType()
        case "tick":
            return TickType()
        case "short_tick":
            return ShortTickType()
        case "replicated_id":
            return ReplicatedIdType()
        case "bits":
            parser.expect("(")
            count, location = parser.integer("số bit")
            parser.expect(")")
            if not 1 <= count <= 64:
                raise SchemaError(location, f"bits({count}): số bit phải từ 1 tới 64")
            return BitsType(count)
        case "int":
            parser.expect("[")
            lo, lo_location = parser.integer("min")
            parser.expect(",")
            hi, _ = parser.integer("max")
            parser.expect("]")
            if not I64_MIN <= lo <= hi <= I64_MAX:
                raise SchemaError(lo_location, f"int[{lo}, {hi}]: cần min ≤ max, cả hai trong i64")
            return IntType(lo, hi)
        case "quantized":
            return _parse_quantized(parser, token.location)
        case "bytes" | "string" | "array":
            parser.expect("[")
            size, location = parser.integer("cỡ tối đa")
            parser.expect("]")
            if not 1 <= size <= MAX_LENGTH:
                raise SchemaError(location, f"{token.text}[{size}]: cỡ phải từ 1 tới {MAX_LENGTH}")
            if token.text == "bytes":
                return BytesType(size)
            if token.text == "string":
                return StringType(size)
            parser.expect("of")
            return ArrayType(size, _parse_type(parser))
    if TYPE_NAME.fullmatch(token.text):
        return NamedType(token.text, token.location)
    raise SchemaError(token.location, f"kiểu lạ '{token.text}'")


def _parse_quantized(parser: _Parser, location: Location) -> QuantizedType:
    parser.expect("[")
    lo, lo_text, _ = parser.real("min")
    parser.expect(",")
    hi, hi_text, _ = parser.real("max")
    parser.expect(",")
    step, step_text, _ = parser.real("step")
    parser.expect("]")
    text = f"quantized[{lo_text}, {hi_text}, {step_text}]"
    if not (math.isfinite(lo) and math.isfinite(hi) and math.isfinite(step)
            and math.isfinite(hi - lo)):
        raise SchemaError(location, f"{text}: mọi số phải hữu hạn")
    if not lo < hi or not step > 0:
        raise SchemaError(location, f"{text}: cần min < max và step > 0")
    quantized = QuantizedType(lo, hi, step, (lo_text, hi_text, step_text))
    if not 1 <= quantized.max_index <= MAX_QUANTIZATION_INDEX:
        raise SchemaError(location,
                          f"{text}: ceil((max − min) / step) phải từ 1 tới 2^32, là "
                          f"{quantized.max_index}")
    return quantized


def _parse_struct(parser: _Parser, location: Location) -> Struct:
    name = _type_name(parser)
    parser.expect("{")
    fields = _parse_fields(parser)
    if not fields:
        raise SchemaError(name.location, f"struct '{name.text}' không có trường nào")
    return Struct(name.text, fields, location)


def _parse_id(parser: _Parser) -> int:
    parser.expect("=")
    value, location = parser.integer("id")
    if not 1 <= value <= MAX_ID:
        raise SchemaError(location, f"id {value} phải từ 1 tới {MAX_ID}")
    return value


def _parse_attributes(parser: _Parser, allowed: tuple[str, ...]) -> dict[str, tuple]:
    """Thuộc tính đứng trước trường: một tên không theo sau bởi ':'."""
    found: dict[str, tuple] = {}
    while parser.peek().kind == "name" and parser.peek(1).text != ":":
        token = parser.next()
        if token.text not in allowed:
            raise SchemaError(token.location, f"thuộc tính lạ '{token.text}'")
        if token.text in found:
            raise SchemaError(token.location, f"thuộc tính '{token.text}' khai hai lần")
        found[token.text] = (_parse_attribute(parser, token), token.location)
    return found


def _parse_attribute(parser: _Parser, keyword: Token) -> object:
    match keyword.text:
        case "channel":
            value = parser.name("kênh")
            if value.text not in CHANNELS:
                raise SchemaError(value.location, f"kênh lạ '{value.text}'")
            return value.text
        case "from":
            value = parser.name("bên gửi")
            if value.text not in SENDERS:
                raise SchemaError(value.location, f"bên gửi phải là client hay server, gặp "
                                                  f"'{value.text}'")
            return value.text
        case "rate":
            rate, rate_location = parser.integer("tốc độ")
            parser.expect("/")
            parser.expect("s")
            parser.expect("burst")
            burst, burst_location = parser.integer("số lượt dồn")
            if not 1 <= rate <= MAX_RATE:
                raise SchemaError(rate_location, f"tốc độ {rate} phải từ 1 tới {MAX_RATE}")
            if not 1 <= burst <= MAX_BURST:
                raise SchemaError(burst_location, f"burst {burst} phải từ 1 tới {MAX_BURST}")
            return (rate, burst)
        case _:  # stream
            value = parser.name("tên stream")
            if not FIELD_NAME.fullmatch(value.text):
                raise SchemaError(value.location,
                                  f"tên stream '{value.text}' phải viết snake_case")
            return value.text


def _require(found: dict[str, tuple], names: tuple[str, ...], location: Location,
             what: str) -> None:
    for name in names:
        if name not in found:
            raise SchemaError(location, f"{what} thiếu thuộc tính '{name}'")


def _parse_message(parser: _Parser, location: Location) -> Message:
    name = _type_name(parser)
    message_id = _parse_id(parser)
    parser.expect("{")
    found = _parse_attributes(parser, ("channel", "from", "rate"))
    _require(found, ("channel", "from", "rate"), name.location, f"message '{name.text}'")
    fields = _parse_fields(parser)
    rate, burst = found["rate"][0]
    return Message(name.text, message_id, found["channel"][0], found["from"][0], rate, burst,
                   fields, location)


def _parse_event(parser: _Parser, location: Location) -> EventDecl:
    name = _type_name(parser)
    event_id = _parse_id(parser)
    parser.expect("{")
    found = _parse_attributes(parser, ("stream",))
    _require(found, ("stream",), name.location, f"event '{name.text}'")
    fields = _parse_fields(parser)
    return EventDecl(name.text, event_id, found["stream"][0], fields, location)


def _check_unique(names: Iterable[str], locations: Iterable[Location], what: str) -> None:
    seen: set[str] = set()
    for name, location in zip(names, locations, strict=True):
        if name in seen:
            raise SchemaError(location, f"{what} '{name}' trùng")
        seen.add(name)


def _resolve(protocol: Protocol, type_: Type) -> None:
    """Tên kiểu phải là enum hay struct đã khai."""
    if isinstance(type_, ArrayType):
        _resolve(protocol, type_.element)
    elif isinstance(type_, NamedType):
        try:
            protocol.lookup(type_.name)
        except KeyError:
            raise SchemaError(type_.location,
                              f"'{type_.name}' không phải enum hay struct đã khai") from None


def walk(type_: Type) -> Iterable[Type]:
    """type_ rồi kiểu phần tử của nó, nếu là mảng (lồng bao nhiêu tầng cũng được)."""
    yield type_
    if isinstance(type_, ArrayType):
        yield from walk(type_.element)


def _referenced_structs(type_: Type) -> Iterable[NamedType]:
    return (t for t in walk(type_) if isinstance(t, NamedType))


def _check_recursion(protocol: Protocol) -> None:
    structs = {s.name: s for s in protocol.structs}
    done: set[str] = set()

    def visit(struct: Struct, path: tuple[str, ...]) -> None:
        if struct.name in done:
            return
        for field in struct.fields:
            for ref in _referenced_structs(field.type):
                target = structs.get(ref.name)
                if target is None:
                    continue
                if target.name in path or target.name == struct.name:
                    raise SchemaError(ref.location,
                                      f"struct '{target.name}' chứa chính nó qua "
                                      f"{' → '.join((*path, struct.name, target.name))}")
                visit(target, (*path, struct.name))
        done.add(struct.name)

    for struct in protocol.structs:
        visit(struct, ())


def _check_used(protocol: Protocol) -> None:
    """Mọi enum và struct được ít nhất một message hay event dùng, trực tiếp hay qua struct."""
    used: set[str] = set()

    def mark(type_: Type) -> None:
        for ref in _referenced_structs(type_):
            if ref.name in used:
                continue
            used.add(ref.name)
            decl = protocol.lookup(ref.name)
            if isinstance(decl, Struct):
                for field in decl.fields:
                    mark(field.type)

    for decl in (*protocol.messages, *protocol.events):
        for field in decl.fields:
            mark(field.type)
    for unused in (d for d in (*protocol.enums, *protocol.structs) if d.name not in used):
        raise SchemaError(unused.location,
                          f"'{unused.name}' không được message hay event nào dùng")


def _limit(decl: Message | EventDecl) -> int:
    if isinstance(decl, EventDecl):
        return EVENT_LIMIT
    return UNRELIABLE_LIMIT if decl.channel in ("unreliable", "sequenced") else RELIABLE_LIMIT


def _validate(decls: _Declarations) -> Protocol:
    if not decls.versions:
        raise SchemaError(Location("<protocol>", 1, 1), "thiếu khai báo version")
    if len(decls.versions) > 1:
        raise SchemaError(decls.versions[1][1], "version khai hơn một lần")
    version, version_location = decls.versions[0]
    if not 1 <= version <= MAX_VERSION:
        raise SchemaError(version_location, f"version {version} phải từ 1 tới {MAX_VERSION}")
    named = [*decls.enums, *decls.structs, *decls.messages, *decls.events]
    _check_unique((d.name for d in named), (d.location for d in named), "tên kiểu")
    _check_unique((str(m.id) for m in decls.messages), (m.location for m in decls.messages),
                  "id của message")
    _check_unique((str(e.id) for e in decls.events), (e.location for e in decls.events),
                  "id của event")
    protocol = Protocol(version, tuple(decls.enums), tuple(decls.structs),
                        tuple(sorted(decls.messages, key=lambda m: m.id)),
                        tuple(sorted(decls.events, key=lambda e: e.id)))
    for decl in (*protocol.structs, *protocol.messages, *protocol.events):
        for field in decl.fields:
            _resolve(protocol, field.type)
    _check_recursion(protocol)
    _check_used(protocol)
    for decl in (*protocol.messages, *protocol.events):
        size = protocol.max_encoded_size(decl)
        if size > _limit(decl):
            raise SchemaError(decl.location,
                              f"'{decl.name}' dài tới {size} byte, vượt {_limit(decl)} byte của "
                              f"{'event' if isinstance(decl, EventDecl) else decl.channel}")
    return protocol


def load(paths: Sequence[pathlib.Path], root: pathlib.Path | None = None) -> Protocol:
    """Đọc và kiểm mọi tệp schema như một protocol; tên tệp trong lỗi tương đối theo root."""
    decls = _Declarations()
    for path in sorted(paths):
        shown = path.relative_to(root).as_posix() if root is not None else path.as_posix()
        try:
            text = path.read_bytes().decode("utf-8")
        except UnicodeDecodeError as error:
            raise SchemaError(Location(shown, 1, 1), f"tệp không phải UTF-8: {error}") from None
        _parse_file(text, shown, decls)
    return _validate(decls)


def loads(sources: dict[str, str]) -> Protocol:
    """Như load, cho nội dung trong bộ nhớ (test): {tên tệp: nội dung}."""
    decls = _Declarations()
    for path in sorted(sources):
        _parse_file(sources[path], path, decls)
    return _validate(decls)
