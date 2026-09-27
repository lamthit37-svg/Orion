# Kênh tin nhắn — định dạng phiên bản 1

Mỗi tin nhắn của protocol thuộc đúng một kênh, khai trong schema (ADR 0004 mục 3, CLAUDE.md X.10):
`unreliable`, `sequenced`, `reliable_ordered`, `reliable_unordered`. Lớp kênh gom tin nhắn của một
kết nối thành gói, xác nhận gói đã nhận, gửi lại phần tin nhắn tin cậy chưa được xác nhận, cắt tin
nhắn lớn thành mảnh rồi ghép lại. Gói của lớp này là dữ liệu tầng trên (`kind` 1) trong gói dữ liệu
của transport (`transport.md`, mục Kết nối), tối đa 1170 byte. Code nằm ở `engine/net/channels.hpp`.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này và code trong cùng commit (CLAUDE.md
X.15).

## Quy ước

Gói ghi bằng bitstream của `protocol.md` (bit thấp trước, byte cuối đệm bit 0, không byte thừa),
với các kiểu `bits(n)`, `bool`, `int[min, max]`, `bytes[max]` của bảng Kiểu ở đó. Số thứ tự 16 bit
so sánh theo vòng: `a` mới hơn `b` khi `(a − b) mod 2^16` nằm trong `[1, 2^15)`.

## Kênh

| Kênh | Số | Hứa gì |
|---|---|---|
| `unreliable` | 0 | không gì: gói mất thì tin nhắn mất |
| `sequenced` | 1 | không gửi lại; tin nhắn cũ hơn tin nhắn đã giao của kênh bị bỏ |
| `reliable_ordered` | 2 | giao đúng một lần, theo thứ tự gửi |
| `reliable_unordered` | 3 | giao đúng một lần, ngay khi đủ mảnh |

Tin nhắn `unreliable` và `sequenced` dài 1 tới 1150 byte, để luôn vừa một gói cùng header (còn dư
vài byte). Tin nhắn tin cậy dài tới 1024 byte đi trong một mảnh; dài hơn thì cắt thành mảnh 1024
byte, mảnh cuối 1 tới 1024 byte, tối đa 32 mảnh. Mỗi đầu khai `receive_buffer_size`, 1 tới
1 048 576: số byte nó giữ cho tin nhắn chưa giao của mỗi kênh tin cậy (mục Bộ đệm). Bên gửi dùng
đúng số đó của bên kia; tin nhắn tin cậy dài tối đa `min(receive_buffer_size, 32 768)` byte. Số này
là hằng của protocol ở cả hai đầu, không thương lượng trên dây.

## Gói

| Trường | Kiểu | Luật |
|---|---|---|
| `sequence` | `bits(16)` | số thứ tự gói của lớp kênh, tăng 1 mỗi gói, theo vòng |
| `has_ack` | `bool` | đã nhận ít nhất một gói của bên kia |
| `ack` | `bits(16)` | chỉ khi `has_ack`: `sequence` mới nhất đã nhận |
| `ack_bits` | `bits(32)` | chỉ khi `has_ack`: bit `i` là 1 khi đã nhận gói `ack − 1 − i` |
| tin nhắn | | lặp: `bool` 1 rồi một tin nhắn; kết thúc bằng `bool` 0 |

Một gói chở tối đa 64 tin nhắn. Tin nhắn:

| Trường | Kiểu | Có khi |
|---|---|---|
| `channel` | `bits(2)` | luôn có |
| `sequence` | `bits(16)` | `sequenced`: số thứ tự của tin nhắn trong kênh |
| `message_id` | `bits(16)` | kênh tin cậy: id của tin nhắn trong kênh, tăng 1 mỗi tin nhắn |
| `fragment_count` | `int[1, 32]` | kênh tin cậy |
| `fragment_index` | `int[0, 31]` | kênh tin cậy; nhỏ hơn `fragment_count` |
| `offset` | `bits(20)` | kênh tin cậy: đầu đoạn của tin nhắn trong bộ đệm nhận (mục Bộ đệm) |
| `data` | `bytes[1170]` | luôn có, ít nhất 1 byte |

`data` của `unreliable` và `sequenced` tối đa 1150 byte; của một mảnh tin cậy tối đa 1024 byte, và
mọi mảnh trừ mảnh cuối dài đúng 1024 byte. Mọi mảnh của cùng một tin nhắn (cùng kênh, cùng
`message_id`) khai cùng `fragment_count` và cùng `offset`, dù nằm trong cùng gói hay ở các gói khác
nhau. Đoạn của tin nhắn nằm trọn trong bộ đệm nhận: `offset` cộng cỡ tin nhắn (biết được từ mảnh
cuối, hay ít nhất `(fragment_count − 1) × 1024 + 1`) không vượt `receive_buffer_size` của bên đọc.
Mỗi dãy bit có đúng một cách đọc; bên đọc từ chối cả gói khi vi phạm bất kỳ luật nào ở trên.

## Bộ đệm

Mỗi kênh tin cậy, mỗi chiều, có một bộ đệm `receive_buffer_size` byte ở bên nhận và một bộ đệm
cùng cỡ ở bên gửi. Bên gửi đặt mỗi tin nhắn vào một đoạn liền của bộ đệm như một hàng đợi vòng: tin
nhắn mới đứng ngay sau tin nhắn mới nhất đang bay, hay ở đầu bộ đệm khi phần cuối không đủ chỗ; đoạn
của một tin nhắn chỉ được dùng lại khi nó và mọi tin nhắn cũ hơn đã được xác nhận hết. Không còn đoạn
liền đủ lớn thì gửi thêm bị từ chối (`ResourceExhausted`). `offset` trên dây là đầu đoạn đó.

Mọi tin nhắn bên nhận đang giữ đều nằm trong cửa sổ của bên gửi, và các đoạn trong cửa sổ không chồng
lên nhau, nên bên nhận chép mảnh thẳng vào `offset + fragment_index × 1024` của bộ đệm của nó mà
không cần cấp phát hay sắp xếp gì. Bộ nhớ của một kênh vì vậy là ngân sách byte đã khai, không tỉ lệ
với cỡ tin nhắn lớn nhất. Bên gửi không trung thực chỉ làm hỏng tin nhắn của chính nó: mọi đoạn vẫn
được kiểm nằm trong bộ đệm.

## Xác nhận và gửi lại

- Bên nhận nhớ `sequence` mới nhất của bên kia và 32 gói trước nó, rồi ghi vào mỗi gói gửi đi.
  Xác nhận là cho cả gói: mọi mảnh tin cậy trong gói được xác nhận coi như đã tới.
- Bên gửi nhớ 64 gói gần nhất: mảnh nào nằm trong gói nào, gửi lúc nào. Gói được xác nhận lần đầu
  cho một mẫu RTT; RTT làm mượt bằng trung bình trượt hệ số 1/8, bắt đầu từ 100 ms.
- Mảnh tin cậy chưa được xác nhận được gửi lại khi đã quá `max(50 ms, 1,5 × RTT)` từ lần gửi trước.
- Mỗi kênh tin cậy có tối đa 64 tin nhắn chưa được xác nhận hết; đầy thì gửi thêm bị từ chối
  (`ResourceExhausted`) để tầng trên giảm nhịp.
- Bên nhận nhận tin nhắn có id trong 64 id kể từ id đầu tiên chưa giao; id cũ hơn là bản lặp, bị bỏ;
  id xa hơn bị bỏ cùng cả gói. Bên nhận không bao giờ bỏ một mảnh nằm trong cửa sổ, vì gói chứa nó
  đã được xác nhận.
- Chỉ gói có mảnh tin cậy mới đòi xác nhận. Có xác nhận chưa gửi mà 25 ms chưa có gói nào đi thì
  gửi một gói chỉ có xác nhận; gói đó không đòi xác nhận lại.
- Mỗi gói xếp trước các tin nhắn `unreliable` và `sequenced` đang chờ, theo thứ tự gửi, rồi tới các
  mảnh tin cậy tới hạn (chưa gửi lần nào, hay quá hạn gửi lại): `reliable_ordered` trước
  `reliable_unordered`, trong mỗi kênh id cũ trước.

Các số ở mục này là chính sách, không phải số đo.

## Lỗi

| Tình huống | Mã lỗi |
|---|---|
| Hết bit giữa chừng | `OutOfRange` |
| Vi phạm luật của bảng Gói hay Tin nhắn, đoạn ngoài bộ đệm nhận, id tin nhắn ngoài cửa sổ, quá 64 tin nhắn | `InvalidArgument` |
