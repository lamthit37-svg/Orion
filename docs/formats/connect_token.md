# Connect token — định dạng phiên bản 1

Connect token là giấy vào cổng của kênh realtime (CLAUDE.md X.9, ARCH §4.6). Dịch vụ `auth` cấp nó
sau khi người chơi đăng nhập, gateway kiểm chữ ký tại chỗ bằng khoá công khai của `auth`, không cần
hỏi lại `auth`. Token gắn với khoá X25519 mà client dùng để bắt tay, nên người lấy được token mà
không có khoá bí mật tương ứng thì không hoàn tất được bắt tay. Code nằm ở
`engine/net/connect_token.hpp`; `auth` và gateway gọi cùng code đó (CLAUDE.md X.14).

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: tăng `version`, sửa tài liệu này và code trong cùng commit
(CLAUDE.md X.15).

## Quy ước

Như `manifest.md`: số nguyên little-endian, không có byte đệm ngầm. Chữ ký là Ed25519 (RFC 8032)
qua `engine/crypto/sign.hpp`. Thời điểm là micro giây từ Unix epoch theo UTC, kiểu `i64`
(`WallTime`, ADR 0002).

## Luồng

1. Client tạo một cặp khoá X25519 mới cho lần đăng nhập, gửi khoá công khai cùng yêu cầu đăng nhập
   tới `auth` qua HTTPS.
2. `auth` kiểm mật khẩu rồi ký token gồm id tài khoản, hạn dùng, khoá X25519 của client và khoá
   định danh của cụm server mà client được vào.
3. Client gửi token trong gói bắt tay tới gateway. Gateway kiểm token theo mục "Kiểm", rồi trao khoá
   với đúng khoá X25519 ghi trong token.
4. Client kiểm chữ ký của gateway trong bắt tay bằng `server_key` đọc từ chính token của nó. Client
   không kiểm chữ ký của `auth`: token đến từ `auth` qua TLS.

## Bố cục

Cỡ cố định 196 byte.

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u8[8]` | `magic` | `ORIONTOK` |
| 8 | `u16` | `version` | `1` |
| 10 | `u16` | `flags` | `0` |
| 12 | `u64` | `account_id` | id tài khoản (ADR 0006), khác 0 |
| 20 | `i64` | `issued_at` | lúc ký |
| 28 | `i64` | `expires_at` | sau `issued_at`, và `expires_at − issued_at` không quá 120 giây |
| 36 | `u8[32]` | `client_key` | khoá công khai X25519 của client cho bắt tay |
| 68 | `u8[32]` | `server_key` | khoá công khai Ed25519 định danh cụm server mà token dành cho |
| 100 | `u8[32]` | `signer` | khoá công khai Ed25519 của `auth` đã ký |
| 132 | `u8[64]` | `signature` | Ed25519 của `signer` trên 132 byte đầu |

`client_key` không được kiểm ở đây: bắt tay từ chối khoá cho bí mật chung toàn 0 (khoá bậc thấp,
`engine/crypto/key_exchange.hpp`).

## Kiểm

Gateway kiểm theo thứ tự, dừng ở lỗi đầu tiên:

1. Cỡ, `magic`, `version`, `flags`.
2. `signer` nằm trong danh sách khoá tin cậy mà bên gọi đưa vào; so bằng so sánh thời gian hằng với
   cả danh sách, không dừng sớm. Danh sách có nhiều khoá để xoay khoá của `auth` không làm rớt token
   đang lưu hành.
3. Chữ ký đúng. Chỉ sau bước này các trường còn lại mới được đọc.
4. `account_id` khác 0; `expires_at` sau `issued_at` và không quá 120 giây sau nó.
5. `server_key` bằng khoá định danh của chính cụm server đang kiểm.
6. `issued_at` không sau `now` quá 10 giây, và `now` trước `expires_at`.

Client đọc token bằng `inspect_connect_token`: bước 1 và bước 4, không kiểm chữ ký.

## Lỗi

Parser là hàm toàn phần (CLAUDE.md X.9): mọi dãy byte cho ra một token đã kiểm hoặc một lỗi.

| Tình huống | Mã lỗi |
|---|---|
| Cỡ khác 196 byte, hoặc `magic` sai | `InvalidArgument` |
| `version` khác 1 | `Unimplemented` |
| `signer` không nằm trong danh sách khoá tin cậy | `Unauthenticated` |
| `server_key` là của cụm server khác | `PermissionDenied` |
| `issued_at` sau `now` quá 10 giây | `FailedPrecondition` |
| `now` không trước `expires_at` | `DeadlineExceeded` |
| Chữ ký sai, `flags` khác 0, vi phạm luật ở bước 4 | `DataLoss` |

## Chính sách

Ba con số dưới đây là chính sách, không phải số đo; đổi chúng không đổi định dạng nhưng phải sửa
tài liệu này cùng code.

- Tuổi thọ tối đa 120 giây: token chỉ dùng lúc bắt tay, client xin token ngay trước khi kết nối.
  Kết nối đã lập không phụ thuộc hạn của token. `auth` chọn tuổi thọ thật trong giới hạn này.
- Lệch đồng hồ cho phép 10 giây giữa máy `auth` và máy gateway, chỉ áp cho `issued_at`.
- Một token chỉ lập được một kết nối; gateway nhớ token đã dùng tới khi nó hết hạn. Đó là việc của
  bước bắt tay, không phải của phần kiểm token.

## Riêng tư

Token không phải bí mật: nó đi trên UDP ở dạng rõ trong gói bắt tay, và người nghe lén thấy được
`account_id`. Nó không cho người lấy được nó quyền gì khi không có khoá bí mật X25519 của client.
