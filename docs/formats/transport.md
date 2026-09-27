# Transport — định dạng phiên bản 1

Kênh realtime giữa client và gateway chạy trên UDP (ARCH §4.5, CLAUDE.md X.9): mỗi gói sau bắt tay
được mã hoá và xác thực bằng AEAD, nonce lấy từ số thứ tự của gói, và một cửa sổ chống replay bỏ gói
lặp lại. Kết nối được nhận diện bằng connection id do server cấp, không bằng cặp IP:port. Code nằm ở
`engine/net/packet.hpp` và `engine/net/replay_window.hpp`.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này và code trong cùng commit (CLAUDE.md
X.15). Client và cụm server luôn được cập nhật cùng nhau (ADR 0004), nên không có đàm phán phiên bản
của transport.

## Quy ước

Số nguyên little-endian, không có byte đệm ngầm. AEAD là ChaCha20-Poly1305 bản IETF (RFC 8439) qua
`engine/crypto/aead.hpp`: khoá 32 byte, nonce 12 byte, tag 16 byte.

## Gói

Một gói UDP chở đúng một gói transport, dài tối đa 1200 byte: vừa MTU 1280 byte tối thiểu của IPv6
sau 40 byte header IPv6 và 8 byte header UDP, còn dư 32 byte. Byte đầu là `prefix`; 4 bit thấp của nó
là loại gói.

| Loại | Gói |
|---|---|
| 1 tới 5 | dành cho bắt tay |
| 6 | dữ liệu |

Loại khác bị từ chối.

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

## Cửa sổ chống replay

Mỗi chiều nhận của một kết nối nhớ số thứ tự lớn nhất đã nhận và 1024 số ngay trước nó (cách của
RFC 6479). Gói có `sequence` lớn hơn số lớn nhất luôn qua; gói trong 1024 số trước số lớn nhất qua
nếu số đó chưa nhận; gói cũ hơn nữa bị bỏ dù hợp lệ. Cỡ 1024 là chính sách, không phải số đo: ở 60
gói mỗi giây, một gói phải tới trễ hơn 17 giây so với gói mới nhất mới bị bỏ vì cửa sổ.

## Lỗi

Parser là hàm toàn phần (CLAUDE.md X.9): mọi dãy byte cho ra một gói hoặc một lỗi.

| Tình huống | Mã lỗi |
|---|---|
| Ngắn hơn header và tag, dài hơn 1200 byte, `prefix` sai, `connection_id` là 0, `sequence` không ngắn nhất | `InvalidArgument` |
| Tag sai | `DataLoss` |
