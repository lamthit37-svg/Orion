# Transport — định dạng phiên bản 1

Kênh realtime giữa client và gateway chạy trên UDP (ARCH §4.5, CLAUDE.md X.9). Bắt tay kiểm connect
token (`connect_token.md`), chứng minh client nhận được gói ở địa chỉ nó khai bằng một cookie không
trạng thái, rồi trao khoá X25519 mà server ký bằng khoá định danh của cụm. Sau đó mỗi gói được mã
hoá và xác thực bằng AEAD, nonce lấy từ số thứ tự của gói, và một cửa sổ chống replay bỏ gói lặp
lại. Kết nối được nhận diện bằng connection id do server cấp, không bằng cặp IP:port. Code nằm ở
`engine/net/`: `handshake.hpp`, `packet.hpp`, `replay_window.hpp` cho định dạng;
`client_transport.hpp` và `server_transport.hpp` cho hai máy trạng thái.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này và code trong cùng commit (CLAUDE.md
X.15). Client và cụm server luôn được cập nhật cùng nhau (ADR 0004), nên không có đàm phán phiên bản
của transport.

## Quy ước

Số nguyên little-endian, không có byte đệm ngầm. AEAD là ChaCha20-Poly1305 bản IETF (RFC 8439) qua
`engine/crypto/aead.hpp`: khoá 32 byte, nonce 12 byte, tag 16 byte. `hash(x)` là BLAKE2b 256 bit
không khoá; chữ ký là Ed25519 (RFC 8032); trao khoá là X25519 theo `crypto_kx` của libsodium
(`engine/crypto/key_exchange.hpp`).

## Gói

Một gói UDP chở đúng một gói transport, dài tối đa 1200 byte: vừa MTU 1280 byte tối thiểu của IPv6
sau 40 byte header IPv6 và 8 byte header UDP, còn dư 32 byte. Byte đầu là `prefix`; 4 bit thấp của nó
là loại gói.

| Loại | Gói | Chiều | Cỡ |
|---|---|---|---|
| 1 | `REQUEST` | client → server | 1200 byte |
| 2 | `CHALLENGE` | server → client | 49 byte |
| 3 | `RESPONSE` | client → server | 249 byte |
| 4 | `ACCEPT` | server → client | 117 byte |
| 5 | `REJECT` | server → client | 18 byte |
| 6 | dữ liệu | hai chiều | 22 tới 1200 byte |

Loại khác bị từ chối. Gói bắt tay có 4 bit cao của `prefix` bằng 0 và đúng cỡ trong bảng.

## Bắt tay

```
client                                        server
  REQUEST(version, nonce, token, đệm)   ──►   kiểm cỡ và version; không giữ trạng thái
                                        ◄──   CHALLENGE(nonce, cookie)
  RESPONSE(version, nonce, cookie, token) ─►  kiểm cookie, rồi token; tạo kết nối chờ
                                        ◄──   ACCEPT(nonce, connection_id, khoá X25519, chữ ký)
  kiểm chữ ký bằng server_key của token
  gói dữ liệu đầu tiên                  ──►   xác nhận kết nối
```

`nonce` là 16 byte ngẫu nhiên client chọn cho mỗi lần thử kết nối; mọi gói server gửi trước khi có
khoá đều nhắc lại nó, nên kẻ không thấy được `REQUEST` không giả được `CHALLENGE`, `ACCEPT` hay
`REJECT`. `token_hash` là `hash(connect_token)`.

### REQUEST

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u8` | `prefix` | `1` |
| 1 | `u32` | `protocol_version` | `kProtocolVersion` của schema (ADR 0004) |
| 5 | `u8[16]` | `nonce` | |
| 21 | `u8[196]` | `connect_token` | chỉ kiểm cấu trúc ở bước sau |
| 217 | `u8[983]` | đệm | toàn 0 |

Gói luôn dài 1200 byte, cỡ lớn nhất của transport: nếu đường truyền không chở được gói cỡ đó, bắt
tay hỏng ngay thay vì kết nối hỏng về sau. Nhờ vậy mọi gói server trả lời trước khi xác thực đều nhỏ
hơn nhiều so với gói gây ra nó (chống khuếch đại, X.9).

Server không giữ trạng thái nào cho `REQUEST`. `protocol_version` khác bản của server thì trả
`REJECT` lý do 1 (ADR 0004 mục 5); ngược lại trả `CHALLENGE`.

### CHALLENGE

| Vị trí | Kiểu | Trường |
|---|---|---|
| 0 | `u8` | `prefix` = `2` |
| 1 | `u8[16]` | `nonce` của `REQUEST` |
| 17 | `u8[32]` | `cookie` |

### Cookie

Cookie chứng minh client nhận được gói ở địa chỉ nguồn nó khai, trước khi server làm việc tốn kém
(kiểm chữ ký Ed25519, trao khoá) hay giữ trạng thái. Client coi nó là dãy byte kín.

| Vị trí | Kiểu | Trường |
|---|---|---|
| 0 | `u64` | `cookie_sequence` |
| 8 | `u8[24]` | AEAD của `expires_at` (`i64`, nano giây trên đồng hồ đơn điệu của server) |

Khoá AEAD là khoá cookie của server: 32 byte ngẫu nhiên, thay mới mỗi 60 giây; cookie của khoá
trước vẫn được nhận tới lần thay sau. Nonce là `nonce_from_sequence(1, cookie_sequence)`, với
`cookie_sequence` đếm từ 0 cho mỗi khoá, nên không bao giờ lặp trong một khoá. Associated data là
`protocol_version` (4 byte) ‖ `nonce` (16) ‖ `token_hash` (32) ‖ địa chỉ nguồn (19: họ `4` hay `6`,
16 byte địa chỉ với IPv4 ở 4 byte đầu và phần còn lại là 0, cổng `u16`). Cookie sống 10 giây.

### RESPONSE

| Vị trí | Kiểu | Trường |
|---|---|---|
| 0 | `u8` | `prefix` = `3` |
| 1 | `u32` | `protocol_version` |
| 5 | `u8[16]` | `nonce` |
| 21 | `u8[32]` | `cookie` |
| 53 | `u8[196]` | `connect_token` |

Server kiểm theo thứ tự:

1. Cỡ, `prefix`.
2. Cookie mở được bằng khoá cookie hiện tại hay khoá trước, với associated data dựng từ chính gói
   này và địa chỉ nguồn của nó, và chưa hết hạn. Sai thì bỏ gói, không trả lời: địa chỉ nguồn chưa
   được chứng minh.
3. Token theo `connect_token.md`. Sai thì trả `REJECT` lý do 3 khi token hết hạn, lý do 2 khi sai
   cách khác.
4. Token chưa lập kết nối nào (lý do 4), còn chỗ cho kết nối mới (lý do 5), và khoá X25519 của
   client trao được khoá (lý do 2).

Qua cả bốn bước thì server tạo một kết nối chờ và trả `ACCEPT`. `RESPONSE` lặp lại cho đúng kết nối
chờ đó, từ cùng địa chỉ, nhận lại đúng `ACCEPT` đã gửi.

### ACCEPT

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u8` | `prefix` | `4` |
| 1 | `u8[16]` | `nonce` | của `REQUEST` |
| 17 | `u32` | `connection_id` | khác 0 |
| 21 | `u8[32]` | `server_key_exchange` | khoá công khai X25519 dùng một lần của server |
| 53 | `u8[64]` | `signature` | Ed25519 của khoá định danh cụm server trên bản ghi dưới |

Bản ghi được ký (104 byte): `ORION ACCEPT v1` và một byte 0 (16 byte) ‖ `protocol_version` ‖
`nonce` ‖ `token_hash` ‖ `connection_id` ‖ `server_key_exchange`. Client kiểm chữ ký bằng
`server_key` trong token của nó; ký sai thì bỏ gói. Bản ghi buộc chữ ký vào token, tức vào khoá
X25519 của client, nên kẻ đứng giữa không thay được khoá của server. Hai bên rồi tính khoá phiên
bằng `crypto_kx` từ khoá X25519 của client trong token và `server_key_exchange`.

### REJECT

| Vị trí | Kiểu | Trường |
|---|---|---|
| 0 | `u8` | `prefix` = `5` |
| 1 | `u8[16]` | `nonce` của `REQUEST` |
| 17 | `u8` | `reason` |

| `reason` | Nghĩa |
|---|---|
| 1 | `protocol_version` khác bản của server: client phải cập nhật |
| 2 | token sai, hay khoá X25519 của client không trao được khoá |
| 3 | token hết hạn: client xin token mới |
| 4 | token đã lập một kết nối |
| 5 | server hết chỗ |

`REJECT` không được xác thực: client chỉ tin nó khi `nonce` khớp lần thử đang chạy.

## Gói dữ liệu

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u8` | `prefix` | bit 0..3 là `6`; bit 4..6 là `n − 1`; bit 7 là `0` |
| 1 | `u32` | `connection_id` | do server cấp lúc bắt tay, khác 0 |
| 5 | `u8[n]` | `sequence` | `n` byte thấp của số thứ tự `u64`; `n` là số nhỏ nhất từ 1 tới 8 đủ chứa nó |
| 5 + n | `u8[L + 16]` | `ciphertext` | payload `L` byte đã mã hoá, rồi tag |

Header là `5 + n` byte đầu. Cỡ gói từ `5 + n + 16` tới 1200 byte. Mỗi số thứ tự có đúng một cách mã
hoá: `n` lớn hơn cần thiết bị từ chối.

Bên gửi mã hoá payload bằng khoá chiều gửi của nó (bắt tay trao hai khoá, một cho mỗi chiều), nonce
`nonce_from_sequence(0, sequence)` (4 byte `0` rồi 8 byte `sequence`), và header làm associated
data: sửa bất kỳ byte nào của header cũng làm gói hỏng tag.

### Số thứ tự

Mỗi chiều của một kết nối đánh số gói từ 0, tăng 1 mỗi gói gửi đi, kể cả gói gửi lại nội dung cũ.
Một cặp khoá và nonce không bao giờ được dùng hai lần, nên bên gửi không bao giờ dùng lại một số thứ
tự. Hết số (2^64 gói; ở 1000 gói mỗi giây là khoảng 5,8 × 10^8 năm) thì kết nối phải đóng.

### Nhận

Bên nhận làm theo thứ tự, bỏ gói ở bước đầu tiên không qua:

1. Cỡ, `prefix`, `connection_id` khác 0, `sequence` mã hoá ngắn nhất.
2. Tra kết nối theo `connection_id`.
3. Hỏi cửa sổ chống replay: gói có `sequence` quá cũ hay đã nhận thì bỏ, không giải mã.
4. Mở AEAD. Chỉ gói qua bước này mới được ghi vào cửa sổ chống replay, để gói giả không đẩy được
   cửa sổ đi.

## Kết nối

### Payload

Byte đầu của payload trong gói dữ liệu là `kind`:

| `kind` | Payload |
|---|---|
| 0 | keep-alive: đúng 1 byte |
| 1 | dữ liệu của tầng trên (gói của lớp kênh, `channels.md`): byte `kind` rồi 1 tới 1170 byte |
| 2 | ngắt kết nối: đúng 1 byte |

Payload khác bị bỏ (gói vẫn được tính là đã nhận hợp lệ). 1170 là payload mà mọi số thứ tự chở được
trong 1200 byte, trừ byte `kind`.

### Server

- Kết nối chờ sau `ACCEPT` trở thành kết nối đã xác nhận khi nhận gói dữ liệu hợp lệ đầu tiên; chỉ
  từ lúc đó tầng trên mới biết connection id. Không được xác nhận trong 10 giây thì bị bỏ.
- Gói dữ liệu hợp lệ có số thứ tự lớn hơn mọi số đã nhận chuyển kết nối sang địa chỉ nguồn của nó
  (client đổi mạng, NAT đổi cổng). Gói hợp lệ nhưng cũ hơn, tới từ địa chỉ khác, vẫn được nhận mà
  không chuyển địa chỉ. Kẻ ngoài đường truyền không làm được gói hợp lệ nên không kéo được kết nối
  đi; kẻ trên đường truyền thì vốn đã chặn được gói.
- Một token chỉ lập một kết nối; server nhớ token đã dùng tới khi token hết hạn, kể cả sau khi kết
  nối đóng. Bảng này giữ tối đa 4 lần số kết nối tối đa; đầy thì kết nối mới nhận lý do 5.

### Client

- Gửi `REQUEST` rồi `RESPONSE` lại mỗi 250 ms tới khi có trả lời. `CHALLENGE` mới (cùng `nonce`)
  thay cookie đang dùng. Không xong bắt tay trong 10 giây thì bỏ cuộc; `REJECT` có `nonce` khớp thì
  dừng ngay với lý do đó.
- Sau `ACCEPT` hợp lệ, gửi ngay một keep-alive để server xác nhận kết nối.
- Chỉ nhận gói từ địa chỉ của server.

### Hai bên

- Đã 1 giây không gửi gì thì gửi keep-alive.
- Không nhận gói hợp lệ nào trong 10 giây thì coi kết nối đã mất.
- Bên chủ động ngắt gửi 3 gói ngắt kết nối liên tiếp rồi bỏ trạng thái; bên kia bỏ trạng thái khi
  nhận được một trong số đó, hay khi hết 10 giây im lặng.

Các con số ở mục này là chính sách, không phải số đo.

## Cửa sổ chống replay

Mỗi chiều nhận của một kết nối nhớ số thứ tự lớn nhất đã nhận và 1024 số ngay trước nó (cách của
RFC 6479). Gói có `sequence` lớn hơn số lớn nhất luôn qua; gói trong 1024 số trước số lớn nhất qua
nếu số đó chưa nhận; gói cũ hơn nữa bị bỏ dù hợp lệ. Cỡ 1024 là chính sách, không phải số đo: ở 60
gói mỗi giây, một gói phải tới trễ hơn 17 giây so với gói mới nhất mới bị bỏ vì cửa sổ.

## Lỗi

Parser là hàm toàn phần (CLAUDE.md X.9): mọi dãy byte cho ra một gói hoặc một lỗi.

| Tình huống | Mã lỗi |
|---|---|
| Gói dữ liệu ngắn hơn header và tag, dài hơn 1200 byte, `prefix` sai, `connection_id` là 0, `sequence` không ngắn nhất | `InvalidArgument` |
| Gói bắt tay sai cỡ hay sai `prefix`, đệm của `REQUEST` khác 0, `connection_id` của `ACCEPT` là 0, `reason` của `REJECT` ngoài bảng | `InvalidArgument` |
| Tag sai | `DataLoss` |
