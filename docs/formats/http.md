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

## Kết nối

Server HTTP/1.1 của `server.hpp` (Boost.Beast, ADR 0015) chỉ nói HTTP/1.1 trên TCP thường, sau load
balancer; TLS kết thúc ở load balancer (CLAUDE.md X.14).

- Kết nối giữ lại sau mỗi response (keep-alive), trừ khi: client gửi `Connection: close`; handler
  đặt `Response::close`; đã đủ `max_requests_per_connection` response; hay server tự trả lỗi đọc
  request (mục "Lỗi"). Response cuối khi đó kèm `Connection: close`.
- Request gửi nối nhau (pipelining) được xử lý lần lượt, response theo đúng thứ tự.
- Đủ `max_connections` kết nối thì server ngừng nhận; kết nối mới chờ trong hàng listen của hệ điều
  hành tới khi có kết nối đóng.
- Đóng êm: sau response cuối, server đóng chiều gửi rồi đọc bỏ những gì client còn gửi tới khi
  client đóng, tối đa `linger_timeout` và 1 MiB, để response không bị TCP reset xoá khỏi bộ đệm
  nhận của client.
- Handler chạy trên nhóm worker (`http-worker`), không trên luồng IO (`http-io`; ADR 0007). Hàng đợi
  tới worker có sức chứa `queue_capacity`; đầy thì server trả 503 ngay.

## Giới hạn và hạn

Giá trị mặc định ở `Limits` và `ServerConfig`; mỗi dịch vụ có thể đặt chặt hơn.

| Giới hạn | Mặc định | Vượt thì |
|---|---|---|
| `max_header_bytes`: dòng request cộng mọi header, tính cả dòng trống cuối (256 tới 65 535) | 8 KiB | 431, đóng |
| `max_body_bytes` (tối đa 16 MiB) | 64 KiB | 413 ngay khi đọc xong header, không đọc body, đóng |
| `read_timeout`: từ byte đầu tiên của request tới hết body | 10 s | 408, đóng |
| `idle_timeout`: chờ byte đầu tiên của request, kể cả request đầu của kết nối | 75 s | đóng, không response |
| `write_timeout`: ghi xong một response | 10 s | đóng |
| `linger_timeout`: đóng êm sau response cuối | 2 s | đóng |
| `handler_timeout`: `Request::deadline`, tính từ lúc đọc xong request | 10 s | handler truyền hạn này cho mọi lời gọi ra ngoài (X.14) |
| `max_requests_per_connection` | 1 000 | response cuối kèm `Connection: close` |
| `queue_capacity`: request đã đọc xong chờ worker | 64 | 503, `Retry-After: 1`, giữ kết nối |
| `max_connections` | 1 024 | ngừng nhận kết nối mới |

`idle_timeout` dài hơn idle timeout của load balancer, để load balancer luôn là bên đóng kết nối
rảnh trước. Mọi hạn tính trên đồng hồ đơn điệu của server; kết nối quá hạn khi đồng hồ chạm hạn.
Server quét hạn mỗi `check_interval` (mặc định 100 ms), nên một kết nối có thể sống quá hạn tới
chừng đó.

## Request

Server đọc header trước, kiểm, rồi mới đọc body. Luật 1 tới 3 do parser kiểm trong lúc đọc, theo
thứ tự byte tới; luật 4 tới 9 kiểm theo thứ tự bảng khi header đã đọc xong. Luật đầu tiên bị vi
phạm quyết định response:

| # | Luật | Vi phạm |
|---|---|---|
| 1 | Cú pháp của dòng request và header (RFC 9112): chỉ CRLF, không khoảng trắng trước dấu hai chấm, Content-Length là một số duy nhất; chỉ đọc được HTTP/1.0 và 1.1 | 400 |
| 2 | Dòng request, và phần header, mỗi phần không quá `max_header_bytes` | 431 |
| 3 | Content-Length không quá `max_body_bytes` | 413 |
| 4 | Dòng request cộng header không quá `max_header_bytes` | 431 |
| 5 | Phiên bản là HTTP/1.1 | 505 |
| 6 | Method là GET, HEAD, POST, PUT, PATCH, DELETE hay OPTIONS | 501 |
| 7 | Target dạng origin (`/path?query`) hay dạng tuyệt đối với `http`, `https` và authority không rỗng (RFC 9112 §3.2.2); đúng một header `Host` | 400 |
| 8 | Không có `Transfer-Encoding`: server không bao giờ đọc body chunked hay trailer, client phải gửi Content-Length (ADR 0015, quyết định 5) | 411 |
| 9 | Không có `Expect`: server không gửi 100 Continue; client lặp lại request không kèm `Expect` (RFC 9110 §10.1.1) | 417 |

- Request không có Content-Length có body rỗng.
- Dòng header tiếp nối (obs-fold) được thay bằng khoảng trắng, như RFC 9112 §5.2 cho phép.
- `Request::path()` luôn bắt đầu bằng `/` và chưa giải mã `%XX`; với dạng tuyệt đối, đường dẫn và
  query lấy từ target (không có đường dẫn thì là `/`). Giá trị của `Host` không được kiểm.
- `Request::header(name)` không phân biệt hoa thường và trả giá trị đầu tiên khi header lặp.
- `parse_request` đọc một request từ bộ nhớ theo đúng các luật trên; fuzz target `http_request`
  chạy nó.

## Response

- Dòng trạng thái `HTTP/1.1`; luôn có `Date` (IMF-fixdate, RFC 9110 §5.6.7, từ đồng hồ tường của
  server).
- `Content-Type` là `Response::content_type` (mặc định `application/json`); rỗng thì không gửi.
- `Cache-Control: no-store`, trừ khi handler tự đặt `Cache-Control`.
- Header của handler theo thứ tự nó đặt. Tên là token (RFC 9110 §5.6.2) dài tối đa 256 byte; giá
  trị tối đa 8 KiB, không có ký tự điều khiển nào ngoài tab. Handler không được đặt `Connection`,
  `Content-Length`, `Content-Type`, `Date`, `Keep-Alive`, `Retry-After`, `TE`, `Trailer`,
  `Transfer-Encoding`, `Upgrade`. Vi phạm thì server gửi 500 `internal` thay cho response đó và
  log lỗi (có giới hạn tần suất): không bao giờ ghi ra header sai.
- `Retry-After` tính bằng giây, làm tròn lên, khi `Response::retry_after` khác 0.
- `Content-Length` là cỡ body. 204 không có body và không có Content-Length (RFC 9110 §8.6). Response
  cho HEAD có Content-Length của body mà không có body.

## Lỗi

Response lỗi có body JSON `{"error": "<mã>"}` và `Content-Type: application/json`
(`error_response`). Mã là chuỗi ngắn snake_case; handler đặt mã riêng cho lỗi của nó. Mã server tự
trả:

| Status | Mã | Khi | Kết nối |
|---|---|---|---|
| 400 | `bad_request` | luật 1 hay 7 của mục "Request" | đóng |
| 408 | `request_timeout` | quá `read_timeout` | đóng |
| 411 | `length_required` | có `Transfer-Encoding` | đóng |
| 413 | `content_too_large` | Content-Length quá `max_body_bytes` | đóng |
| 417 | `expectation_failed` | có `Expect` | đóng |
| 431 | `header_too_large` | header quá `max_header_bytes` | đóng |
| 500 | `internal` | handler trả header sai | giữ |
| 501 | `not_implemented` | method khác bảy method nhận | đóng |
| 503 | `overloaded` | hàng đợi tới worker đầy; kèm `Retry-After: 1` | giữ |
| 505 | `version_not_supported` | HTTP/1.0 | đóng |
