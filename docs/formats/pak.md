# Pak — định dạng phiên bản 1

Pak gom các tệp cooked của một nền tảng thành một tệp để phát cho người chơi (ARCH §1.3, §4.4). Mỗi
tệp là một entry nén Zstd riêng, nên đọc được một entry mà không giải nén phần còn lại. Code đọc và
ghi nằm ở `engine/io/pak.hpp`; manifest đã ký trỏ tới pak được mô tả ở `manifest.md`.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: tăng `version`, sửa tài liệu này và code trong cùng commit
(CLAUDE.md X.15).

## Quy ước

- Mọi số nguyên là little-endian, không có byte đệm ngầm giữa các trường.
- `hash(x)` là BLAKE2b 256 bit không khoá (`engine/crypto/hash.hpp`).
- Tên tệp của pak là `hash` của toàn bộ tệp, viết hex thường, cộng đuôi `.pak`.

## Bố cục

| Vùng | Vị trí | Nội dung |
|---|---|---|
| Header | `[0, 64)` | bảng dưới |
| Dữ liệu | `[64, index_offset)` | blob của các entry, theo thứ tự bất kỳ, có thể có khoảng trống |
| Index | `[index_offset, cỡ tệp)` | bảng entry, bảng dictionary, bảng chuỗi, dữ liệu dictionary |

Index nằm cuối tệp và kéo tới đúng cuối tệp: `index_offset + index_size` bằng cỡ tệp.

### Header

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u8[8]` | `magic` | `ORIONPAK` |
| 8 | `u16` | `version` | `1` |
| 10 | `u16` | `flags` | `0` |
| 12 | `u32` | `entry_count` | tối đa 262 144 |
| 16 | `u32` | `dictionary_count` | tối đa 16 |
| 20 | `u32` | `string_table_size` | |
| 24 | `u64` | `index_offset` | ít nhất 64 |
| 32 | `u64` | `index_size` | tối đa 64 MiB |
| 40 | `u8[24]` | `reserved` | toàn 0 |

### Index

Theo thứ tự, không có gì xen giữa:

1. `entry_count` bản ghi entry, 64 byte mỗi bản ghi, xếp tăng dần theo đường dẫn (so từng byte),
   không có hai entry cùng đường dẫn.
2. `dictionary_count` bản ghi dictionary, 8 byte mỗi bản ghi: `u32 offset`, `u32 size`. `offset`
   tính từ đầu vùng dữ liệu dictionary; `size` từ 1 byte tới 1 MiB; vùng của mỗi dictionary nằm trọn
   trong vùng dữ liệu dictionary.
3. Bảng chuỗi, `string_table_size` byte: đường dẫn của các entry, không có ký tự kết thúc.
4. Dữ liệu dictionary: phần còn lại của index.

### Entry

| Vị trí | Kiểu | Trường | Luật |
|---|---|---|---|
| 0 | `u64` | `offset` | vị trí blob trong tệp: `64 <= offset` và `offset + stored_size <= index_offset` |
| 8 | `u64` | `stored_size` | cỡ blob, tối đa 1 GiB |
| 16 | `u64` | `original_size` | cỡ sau khi giải nén, tối đa 1 GiB |
| 24 | `u32` | `path_offset` | vị trí đường dẫn trong bảng chuỗi |
| 28 | `u16` | `path_length` | đường dẫn nằm trọn trong bảng chuỗi và là đường dẫn ảo hợp lệ |
| 30 | `u8` | `compression` | `0` không nén (`stored_size = original_size`, `dictionary = 0`); `1` Zstd |
| 31 | `u8` | `dictionary` | `0` không dùng; `k` là dictionary thứ `k` (đếm từ 1), chỉ khi `compression = 1` |
| 32 | `u8[32]` | `hash` | `hash` của blob đúng như lưu trong tệp |

Blob Zstd là đúng một frame của định dạng hiện hành (magic `0xFD2FB528`) ghi sẵn content size bằng
`original_size`, không có frame nào khác theo sau.

Giới hạn 1 GiB của `stored_size` chặn bộ đệm đọc blob của mỗi luồng, kể cả khi pak được mở ở DEV mà
không có manifest để so `index_hash`. Cooker chỉ nén khi blob nhỏ hơn nội dung, nên blob nó ghi
không bao giờ vượt `original_size`.

## Đường dẫn ảo

Đường dẫn của entry theo luật của `VirtualPath` (`engine/io/path.hpp`): 1 tới 200 byte, chỉ gồm
`a-z`, `0-9`, `.`, `_`, `-` và `/` làm dấu ngăn; không mở đầu hay kết thúc bằng `/`, không có `//`;
không đoạn nào mở đầu hay kết thúc bằng `.`; không đoạn nào có phần trước dấu chấm đầu tiên là tên
thiết bị của Windows (`con`, `prn`, `aux`, `nul`, `com0`–`com9`, `lpt0`–`lpt9`). Tệp rời ở DEV dùng
đúng tên này dưới thư mục cooked, nên một đường dẫn trỏ tới cùng một tệp ở cả hai chế độ.

## Toàn vẹn

- `index_hash` là `hash(header ‖ index)`. Manifest đã ký ghi `index_hash`, `hash` của cả tệp và cỡ
  tệp của từng pak. Khi mount, `engine/io` kiểm cỡ tệp, đọc header và index, so `index_hash` với
  manifest, rồi mới phân tích index.
- Mỗi lần đọc một entry, `engine/io` băm blob và so với `hash` của entry trong index đã kiểm, rồi
  mới giải nén; sau khi giải nén, cỡ phải bằng `original_size`. Bộ giải nén vì vậy chỉ nhận blob do
  cooker sinh ra (ADR 0012), và tệp bị sửa sau khi mount vẫn bị phát hiện ở lần đọc.
- Khi mount không băm lại cả tệp: thời gian đó tỉ lệ với dung lượng game, còn cách trên phủ cùng
  nội dung. Với tốc độ BLAKE2b đo trên máy dựng (64 KiB trong 79,8 µs, tức khoảng 780 MiB/s;
  `engine_crypto_bench`, commit 532a22f), băm lại 20 GiB tốn khoảng 26 giây mỗi lần khởi động.
  `hash` của cả tệp (tên tệp) để trình vá và CDN kiểm tệp tải về.

## Lỗi

Parser là hàm toàn phần (CLAUDE.md X.9): mọi dãy byte cho ra một index hợp lệ hoặc một lỗi.

| Tình huống | Mã lỗi |
|---|---|
| `magic` sai | `InvalidArgument` |
| `version` khác 1 | `Unimplemented` |
| Vi phạm bất kỳ luật nào ở trên, `index_hash` sai, `hash` của entry sai, giải nén hỏng | `DataLoss` |
