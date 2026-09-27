# Protocol — định dạng trên dây

Tin nhắn giữa client và server, và giữa các server, được định nghĩa trong
`game/shared/protocol/*.schema` và mã hoá bằng code do `tools/codegen` sinh ra (ADR 0004). Tài liệu
này mô tả định dạng trên dây (mục Bitstream, Tin nhắn), ngôn ngữ schema và code sinh ra.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: tăng `kProtocolVersion`, sửa tài liệu này và code trong
cùng commit (CLAUDE.md X.10, X.15).

## Bitstream

Mọi tin nhắn là một dãy bit (`engine/net/bitstream.hpp`).

### Thứ tự bit

- Bit đầu tiên của dãy là bit thấp nhất (bit 0) của byte đầu tiên; bit thứ 8 là bit 0 của byte thứ
  hai, và cứ thế.
- Một số `n` bit được ghi bit thấp trước: bit 0 của số là bit đầu tiên được ghi.
- Byte cuối được đệm bằng bit 0 tới đủ 8 bit. Không có gì sau byte cuối.

Ví dụ: ghi `1` trên 1 bit, `2` trên 2 bit, `127` trên 7 bit cho hai byte `fd 03`.

### Kiểu

| Kiểu | Mã hoá |
|---|---|
| `bits(n)` | số không dấu `n` bit, `n` từ 0 tới 64 |
| `bool` | 1 bit: `1` là đúng |
| `int[min, max]` | `value − min` (tính theo modulo 2^64) trên `b(max − min)` bit, với `b(x)` là số bit của `x` (`b(0) = 0`, nên khoảng chỉ có một giá trị không tốn bit nào) |
| `quantized[min, max, step]` | chỉ số `i` từ 0 tới `N = ceil((max − min) / step)` trên `b(N)` bit; giá trị là `min + i · step`, kẹp vào `max` |
| `bytes[max]` | độ dài `L ≤ max` trên `b(max)` bit, rồi `L` byte, mỗi byte 8 bit |
| `string[max]` | như `bytes[max]`; nội dung là UTF-8 hợp lệ |

Lượng tử hoá: bên ghi dùng chỉ số gần nhất của giá trị sau khi kẹp vào `[min, max]`, làm tròn nửa
ra xa 0; NaN cho chỉ số 0. `N` tối đa 2^32. Mọi phép tính là phép IEEE 754 làm tròn đúng (chia,
nhân, cộng; không hợp nhất FMA, CLAUDE.md X.11), nên hai đầu cho cùng giá trị.

### Kiểm khi đọc

Bên đọc là parser của dữ liệu từ mạng: hàm toàn phần (CLAUDE.md X.9). Mỗi dãy bit có đúng một cách
mã hoá, nên kiểm theo bảng:

| Tình huống | Mã lỗi |
|---|---|
| Không đủ bit cho trường tiếp theo | `OutOfRange` |
| `int` vượt `max − min`, chỉ số lượng tử hoá vượt `N`, độ dài vượt `max` | `InvalidArgument` |
| `string` không phải UTF-8 | `InvalidArgument` |
| Sau trường cuối: bit đệm khác 0, hay còn byte thừa | `InvalidArgument` |

## Ngôn ngữ schema

Mọi tệp `game/shared/protocol/*.schema` cùng làm thành một protocol: thứ tự tệp và thứ tự khai báo
không quan trọng, và mọi tên dùng chung một không gian. `tools/codegen/orion_codegen.py` đọc chúng
lúc build và sinh `protocol.hpp`, `protocol.cpp` vào `out/build/<preset>/gen/game/shared/protocol/`;
code sinh ra không được commit hay sửa tay. Codegen từ chối cả protocol khi gặp bất kỳ lỗi nào ở mục
này, báo `tệp:dòng:cột`.

### Từ vựng

- Tệp là UTF-8; `#` mở comment tới hết dòng. Khoảng trắng và xuống dòng chỉ ngăn cách các từ.
- Tên kiểu (`enum`, `struct`, `message`, `event`) và tên giá trị enum viết CamelCase: mỗi từ là một
  chữ hoa rồi ít nhất một chữ thường hay chữ số (`([A-Z][a-z0-9]+)+`), nên `HttpProxy` được còn
  `HTTPProxy` và `X` thì không. Tên trường và tên stream viết snake_case
  (`[a-z][a-z0-9]*(_[a-z0-9]+)*`).
- Tên kiểu không được trùng tên mà code hỗ trợ hay code sinh ra đã dùng: `Tick`, `ShortTick`,
  `ReplicatedId`, `BoundedBytes`, `BoundedString`, `BoundedArray`, `Encoder`, `Decoder`, `Sender`,
  `ClientMessage`, `ServerMessage`, `Event`, `MessageEntry`, `EventEntry`, `Result`, `Error`,
  `ErrorCode`.
- Tên trường không được là từ khoá của C++ (kể cả token thay thế như `and`, cùng `final`,
  `override`, `import`, `module`), macro `errno`, `stdin`, `stdout`, `stderr`, hay kiểu số của
  `engine/core/types.hpp` (`i8` … `i64`, `u8` … `u64`, `f32`, `f64`, `usize`, `isize`): code sinh ra
  đặt tên trường thẳng vào C++. Từ của schema (`channel`, `from`, `rate`, `stream`, `tick`, ...) làm
  tên trường được, vì trong khối một tên có `:` theo sau luôn là trường.
- Số nguyên viết thập phân, có thể có dấu `-`. Số thực (chỉ trong `quantized`) viết thập phân, có
  thể có dấu `-` và phần lẻ, không có dạng mũ.

### Khai báo

```text
version 3

enum ChatChannel : u8 {
    Say = 0
    Party = 1
}

struct WorldPosition {
    x: quantized[-32768, 32768, 0.01]
    y: quantized[-2048, 2048, 0.01]
    z: quantized[-32768, 32768, 0.01]
}

message ChatSend = 20 {
    channel reliable_ordered
    from client
    rate 2/s burst 5
    target: ChatChannel
    text: string[255]
}

event ItemGranted = 1 {
    stream economy
    character_id: bits(64)
    count: int[1, 1000]
}
```

- `version N`: `kProtocolVersion`, từ 1 tới 2^32 − 1; đúng một lần trong cả protocol. Đổi bất kỳ
  schema nào thì phải tăng số này (CLAUDE.md X.10).
- `enum Tên : u8|u16|u32 { TênGiáTrị = số ... }`: ít nhất một giá trị; mọi giá trị có số tường
  minh, vừa kiểu nền; không trùng tên, không trùng số.
- `struct Tên { trường: kiểu ... }`: ít nhất một trường; không trùng tên trường; không chứa chính
  nó, trực tiếp hay qua struct khác.
- `message Tên = id { ... }`: tin nhắn qua mạng. `id` từ 1 tới 65535, không trùng giữa các message.
  Ba thuộc tính bắt buộc, mỗi cái đúng một lần, đứng trước các trường:
  - `channel unreliable|sequenced|reliable_ordered|reliable_unordered`: kênh của lớp kênh
    (`channels.md`);
  - `from client|server`: bên gửi;
  - `rate R/s burst B`: giới hạn tần suất theo kết nối của bên nhận (CLAUDE.md X.9), `R` từ 1 tới
    10^9 lần mỗi giây, `B` từ 1 tới 10^6.
- `event Tên = id { stream tên ... }`: sự kiện giữa các server qua Redis Streams (CLAUDE.md X.10).
  `id` từ 1 tới 65535, không trùng giữa các event; `stream` bắt buộc, đúng một lần.
- Message và event có thể không có trường nào.
- `enum`, `struct`, `message`, `event` dùng chung một không gian tên: không hai khai báo nào trùng
  tên. Mọi `enum` và `struct` phải được ít nhất một message hay event dùng, trực tiếp hay qua struct
  khác: khai báo thừa là dấu hiệu schema viết dở.

### Kiểu

| Kiểu | Trên dây | C++ |
|---|---|---|
| `bool` | `bool` | `bool` |
| `bits(n)`, `n` từ 1 tới 64 | `bits(n)` | kiểu nhỏ nhất trong `u8`, `u16`, `u32`, `u64` chứa `n` bit |
| `int[min, max]`, `min ≤ max`, cả hai trong `i64` | `int[min, max]` | kiểu nhỏ nhất trong `u8` … `u64` (khi `min ≥ 0`) hay `i8` … `i64` chứa khoảng |
| `quantized[min, max, step]` | `quantized[min, max, step]` | `f64` |
| `bytes[max]`, `max` từ 1 tới 65535 | `bytes[max]` | `BoundedBytes<max>` |
| `string[max]`, `max` từ 1 tới 65535 | `string[max]` | `BoundedString<max>` |
| `tick` | `bits(64)` | `Tick` |
| `short_tick` | `bits(16)` | `ShortTick` |
| `replicated_id` | `bits(32)` | `ReplicatedId` |
| `array[max] of T`, `max` từ 1 tới 65535 | độ dài `L ≤ max` trên `b(max)` bit, rồi `L` phần tử | `BoundedArray<T, max>` |
| tên một `enum` | `int[số nhỏ nhất, số lớn nhất]` của enum | `enum class` |
| tên một `struct` | các trường theo thứ tự khai | `struct` |

- `quantized`: `min < max`, `step > 0`, mọi số hữu hạn, và `ceil((max − min) / step)` tối đa 2^32
  (mục Kiểu của Bitstream).
- `short_tick` là 16 bit thấp của một `tick`. Bên nhận khôi phục tick đầy đủ bằng tick gần nhất với
  một tick tham chiếu nó đã biết (ví dụ tick mới nhất nhận được) có cùng 16 bit thấp; khoảng cách
  tới tham chiếu phải dưới 2^15 tick, tức hơn 10 phút ở 50 Hz (ADR 0002).
- `replicated_id` là id nhân bản do zone cấp (ADR 0006): 0 là không có thực thể.
- Mảng lồng mảng và mảng struct được phép; `array[max] of T` với `T` là `array` cũng vậy.

### Giới hạn cỡ

Codegen tính cỡ lớn nhất của mỗi tin nhắn trên dây (mục Tin nhắn) và từ chối protocol khi nó vượt:

| Loại | Cỡ lớn nhất |
|---|---|
| message trên `unreliable`, `sequenced` | 1150 byte (`channels.md`) |
| message trên kênh tin cậy | 32 768 byte (`channels.md`) |
| event | 32 768 byte |

## Tin nhắn

Một message được mã hoá thành đúng một tin nhắn của lớp kênh, và một event thành đúng một mục của
Redis Stream:

1. `id` trên `int[0, M]`, với `M` là id lớn nhất đã khai trong nhóm của nó (message hay event);
2. các trường theo thứ tự khai;
3. đệm bit 0 tới hết byte (mục Thứ tự bit).

Thêm một message có id lớn hơn mọi id cũ làm `M` tăng; khi `b(M)` tăng theo, trường `id` của mọi
message rộng thêm. Đó là một thay đổi protocol như mọi thay đổi khác (tăng `kProtocolVersion`).

Bên đọc kiểm như bảng Kiểm khi đọc, và thêm:

| Tình huống | Mã lỗi |
|---|---|
| `id` không phải id đã khai trong nhóm | `InvalidArgument` |
| Message không phải của bên gửi mà bên đọc chờ (`from`), hay tới trên kênh khác kênh đã khai | `InvalidArgument` |
| Giá trị enum không có trong schema | `InvalidArgument` |

## Mã sinh ra

`protocol.hpp` nằm trong namespace `orion::protocol`, dùng code hỗ trợ viết tay ở
`game/shared/protocol/codec.hpp` (`BoundedBytes`, `BoundedString`, `BoundedArray`, `Tick`,
`ShortTick`, `ReplicatedId`, `Sender`) và có:

- `kProtocolVersion`; `kMaxMessageSize`, `kMaxEventSize`: cỡ lớn nhất của mọi message, mọi event
  (cỡ bộ đệm đủ cho `encode`);
- mỗi enum một `enum class` cùng kiểu nền, mỗi struct, message, event một `struct` cùng tên, trường
  cùng tên và thứ tự. Message có `kId`, `kChannel`, `kSender`, `kRateLimit`, `kMaxEncodedSize`; event
  có `kId`, `kStream`, `kMaxEncodedSize`. Mọi trường có giá trị mặc định, nằm trong khoảng đã khai:
  `false`; 0 với `bits`, `tick`, `short_tick`, `replicated_id`; 0, hay `min` khi 0 ngoài khoảng,
  với `int` và `quantized`; giá trị khai đầu tiên với enum; rỗng với `bytes`, `string`, `array`;
  struct lồng nhau lấy mặc định của từng trường. Nên tin nhắn tạo mặc định luôn mã hoá được, và
  khởi tạo theo tên trường (`MoveInput{.tick = t}`) bỏ qua được trường bất kỳ;
- `encode(tin nhắn, out)` cho mỗi message và event: ghi vào `out`, trả số byte. Lỗi:
  `InvalidArgument` khi một trường ngoài khoảng đã khai (số ngoài `int`, enum không có trong schema,
  `bits(n)` cần hơn `n` bit), `ResourceExhausted` khi `out` nhỏ hơn cỡ cần;
- `ClientMessage`, `ServerMessage`, `Event`: `std::variant` của các message theo `from` và của các
  event (chỉ có khi nhóm đó không rỗng), cùng `decode_client_message(kênh, byte)`,
  `decode_server_message(kênh, byte)`, `decode_event(byte)`.

Code sinh ra không cấp phát: mọi trường có cỡ tối đa nên nằm ngay trong struct (CLAUDE.md X.7).
Bên đọc là hàm toàn phần và kiểm hết tin nhắn rồi mới trả (X.9, X.10).
