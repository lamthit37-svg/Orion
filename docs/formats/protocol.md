# Protocol — định dạng trên dây

Tin nhắn giữa client và server, và giữa các server, được định nghĩa trong
`game/shared/protocol/*.schema` và mã hoá bằng code do `tools/codegen` sinh ra (ADR 0004). Tài liệu
này mô tả định dạng trên dây; phần ngôn ngữ schema được viết cùng commit thêm codegen.

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
