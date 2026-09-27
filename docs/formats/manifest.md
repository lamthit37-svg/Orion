# Manifest — định dạng phiên bản 1

Manifest liệt kê các pak tạo nên một bản build của một nền tảng, có chữ ký Ed25519 của khoá phát
hành (ARCH §1.3, §4.4, §4.6). Bản CHƠI đọc `manifest.bin` cạnh thư mục `data/`, kiểm chữ ký bằng
các khoá công khai biên dịch sẵn trong game, rồi chỉ mount đúng các pak manifest liệt kê (CLAUDE.md
X.9). Dịch vụ `patch` phát cùng định dạng này. Code nằm ở `engine/io/manifest.hpp`.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: tăng `version`, sửa tài liệu này và code trong cùng commit
(CLAUDE.md X.15).

## Quy ước

Như `pak.md`: số nguyên little-endian, không có byte đệm ngầm; `hash(x)` là BLAKE2b 256 bit không
khoá. Chữ ký là Ed25519 (RFC 8032) qua `engine/crypto/sign.hpp`.

## Bố cục

| Vùng | Cỡ | Nội dung |
|---|---|---|
| Header | 64 byte | bảng dưới |
| Bản ghi pak | `pak_count` × 72 byte | theo thứ tự mount |
| Chữ ký | 64 byte | Ed25519 của `signer` trên header ‖ các bản ghi |

Cỡ tệp đúng bằng `64 + 72 × pak_count + 64`.

### Header

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u8[8]` | `magic` | `ORIONMAN` |
| 8 | `u16` | `version` | `1` |
| 10 | `u16` | `flags` | `0` |
| 12 | `u32` | `pak_count` | tối đa 4096 |
| 16 | `u64` | `sequence` | số thứ tự của bản phát hành, tăng dần |
| 24 | `u8[8]` | `platform` | 1 tới 8 ký tự `a-z0-9` (`win64`, `android`, `ios`, `linux`), phần còn lại toàn 0 |
| 32 | `u8[32]` | `signer` | khoá công khai Ed25519 đã ký manifest |

### Bản ghi pak

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u64` | `file_size` | cỡ tệp pak, ít nhất 64 (cỡ header của pak) |
| 8 | `u8[32]` | `file_hash` | `hash` của cả tệp pak; không có hai bản ghi trùng |
| 40 | `u8[32]` | `index_hash` | `index_hash` của pak (`pak.md`, mục Toàn vẹn) |

Tệp pak của bản ghi nằm ở `data/<file_hash viết hex thường>.pak`. Pak mount theo thứ tự bản ghi;
một đường dẫn có trong nhiều pak thì pak đứng sau thắng, để một pak vá nhỏ che được entry của pak
gốc mà không phải tải lại pak đó.

## Kiểm

Theo thứ tự, dừng ở lỗi đầu tiên:

1. Cỡ, `magic`, `version`, `flags`, `pak_count` khớp với cỡ tệp.
2. `signer` nằm trong danh sách khoá tin cậy mà bên gọi đưa vào.
3. Chữ ký đúng. Chỉ sau bước này các trường còn lại mới được đọc.
4. `platform` đúng luật và bằng nền tảng đang chạy; từng bản ghi đúng luật.

`sequence` chống quay lui: trình vá nhớ `sequence` lớn nhất đã cài và từ chối manifest có số nhỏ
hơn, để một manifest cũ còn chữ ký hợp lệ không đưa người chơi về bản có lỗ hổng đã vá. Việc này cần
trạng thái lưu giữa các lần chạy nên thuộc trình vá, không thuộc `engine/io`.

## Lỗi

Parser là hàm toàn phần (CLAUDE.md X.9): mọi dãy byte cho ra một manifest đã kiểm hoặc một lỗi.

| Tình huống | Mã lỗi |
|---|---|
| Ngắn hơn header, hoặc `magic` sai | `InvalidArgument` |
| `version` khác 1 | `Unimplemented` |
| `signer` không nằm trong danh sách khoá tin cậy | `Unauthenticated` |
| `platform` đúng luật nhưng khác nền tảng đang chạy | `FailedPrecondition` |
| Chữ ký sai, vi phạm bất kỳ luật nào khác ở trên | `DataLoss` |
