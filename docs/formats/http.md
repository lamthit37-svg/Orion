# HTTP và JSON của các dịch vụ HTTP

Các dịch vụ HTTP (auth, account, store, patch, admin; ARCH §4.6) nhận và trả JSON qua
`game/server/lib/http` (CLAUDE.md X.14). Tài liệu này là hợp đồng giữa các dịch vụ đó và mọi bên
gọi chúng: client, công cụ vận hành, load balancer.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này và code trong cùng commit (CLAUDE.md
X.15).

## JSON

### Đọc

Body của request được đọc bằng `json::Document::parse` (simdjson, ADR 0014) theo RFC 8259, cộng các
luật sau. Vi phạm là lỗi, không bao giờ là assert (CLAUDE.md X.5).

| Luật | Lỗi |
|---|---|
| UTF-8 hợp lệ ở mọi chỗ; BOM UTF-8 đầu tài liệu được bỏ qua (RFC 8259 cho phép) | `InvalidArgument` |
| Không có nội dung thừa sau giá trị gốc, ngoài khoảng trắng | `InvalidArgument` |
| Một object không có hai thành viên cùng tên | `InvalidArgument` |
| Array và object lồng tối đa 32 cấp | `ResourceExhausted` |
| Tối đa 65 536 giá trị, tính mọi giá trị ở mọi độ sâu | `ResourceExhausted` |
| Tối đa 1 MiB; body HTTP còn bị giới hạn chặt hơn | `ResourceExhausted` |
| Số nguyên vừa 64 bit (có dấu hay không dấu) | `OutOfRange` |

Khoá trùng bị từ chối vì hai parser đọc cùng một body có thể chọn hai giá trị khác nhau (load
balancer, log, dịch vụ). Giá trị gốc có thể là bất kỳ loại nào; parser request của mỗi endpoint đòi
một object.

Đọc một giá trị theo kiểu (`json.hpp`):

- `integer()`: chỉ số viết như số nguyên (`42`, `-0`) và vừa `i64`; `5.0` và `5e0` là số thực, nên
  là `InvalidArgument`; số nguyên không vừa `i64` là `OutOfRange`.
- `real()`: mọi số, đổi sang `f64`.
- `string()`: UTF-8 hợp lệ; có thể chứa U+0000 (viết `\u0000`). Parser request tự giới hạn độ dài
  bằng `string(max_bytes)`.
- Parser request của mỗi endpoint gọi `expect_only` để từ chối trường lạ: client gõ sai tên trường
  nhận lỗi thay vì trường bị bỏ qua lặng lẽ.

### Ghi

`json::Writer` ghi JSON gọn, không khoảng trắng, theo đúng thứ tự gọi.

- Chuỗi escape theo RFC 8259: `"` và `\` có dấu `\` đứng trước; xuống dòng, về đầu dòng, tab là
  `\n`, `\r`, `\t`; ký tự điều khiển khác dưới U+0020 là `\u00XX`; mọi ký tự khác, kể cả U+007F
  và ngoài ASCII, ghi nguyên byte UTF-8. Byte UTF-8 hỏng thành U+FFFD, như log của server, nên kết
  quả luôn là JSON hợp lệ.
- Số nguyên ghi dạng thập phân. Số thực ghi dạng ngắn nhất đọc lại đúng từng bit, và luôn có `.`
  hay số mũ (`1.0`, không phải `1`) để bên đọc vẫn thấy một số thực. NaN và vô cực không ghi được.
