# Migration của PostgreSQL — định dạng và luật áp

Schema của PostgreSQL chỉ đổi qua migration: tệp SQL đánh số trong `db/migrations/`, áp bằng
`orion_migrate`, mỗi tệp một transaction (CLAUDE.md X.14, ARCH §2). Code nằm ở `tools/migrate/`, và
`tools/run/cluster.py up` gọi `orion_migrate` trước khi bật server (ARCH §1).

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này và code trong cùng commit (CLAUDE.md
X.15).

## Tệp

- Tên: `NNNN_<tên>.sql`. `NNNN` là phiên bản, đúng bốn chữ số thập phân, từ `0001`, liên tiếp,
  không lỗ, không trùng. `<tên>` gồm chữ thường `a`–`z`, chữ số và `_`, dài 1 tới 60 ký tự.
- Tệp `.sql` sai tên là lỗi. Tệp đuôi khác (ví dụ `README.md`) bị bỏ qua.
- Nội dung là UTF-8, không rỗng, không có byte 0. Script chạy nguyên khối bằng giao thức simple
  query, trong transaction do `orion_migrate` mở.
- Script không có lệnh kết thúc hay mở transaction (`BEGIN`, `COMMIT`, `ROLLBACK`, `END`), và không
  có lệnh không chạy được trong transaction (`CREATE INDEX CONCURRENTLY`, `VACUUM`,
  `CREATE DATABASE`, `ALTER SYSTEM`).
- Tên đối tượng không kèm schema: migration áp vào schema đầu tiên của `search_path`. Test áp mỗi
  lần vào một schema riêng.
- Migration đã merge thì không sửa (CLAUDE.md X.16.5); đổi schema là thêm một migration mới.
  Checksum làm cho việc sửa bị phát hiện.

## Checksum

BLAKE2b 256 bit (`engine/crypto/hash.hpp`) của đúng các byte trong tệp. `.gitattributes` giữ xuống
dòng LF ở mọi nền tảng, nên checksum của một tệp như nhau trên Linux và Windows.

## Bảng `schema_migrations`

`orion_migrate` tự tạo bảng này nếu chưa có; nó không phải một migration.

| Cột | Kiểu | Luật |
|---|---|---|
| `version` | `integer` | khoá chính, lớn hơn 0 |
| `name` | `text` | phần `<tên>` của tệp |
| `checksum` | `bytea` | đúng 32 byte |
| `applied_at` | `timestamptz` | lúc migration được áp, theo đồng hồ của server |

## Áp

1. Đọc và kiểm mọi tệp theo mục "Tệp". Có một lỗi thì không áp gì.
2. Mọi transaction của `orion_migrate` lấy `pg_advisory_xact_lock` với một khoá cố định trước tiên,
   nên hai lần chạy cùng lúc (hai lần triển khai) nối đuôi nhau thay vì áp một migration hai lần.
3. Kiểm lịch sử: các dòng của `schema_migrations` phải là phiên bản `1` tới `k` liên tiếp, mỗi dòng
   khớp tên và checksum của tệp cùng phiên bản. Có phiên bản lớn hơn số tệp là lỗi: DB mới hơn code.
4. Mỗi migration chưa áp, theo thứ tự tăng dần, trong một transaction riêng: lấy khoá; kiểm lại nó
   chưa được áp (lần chạy khác có thể vừa áp; khi đó chỉ kiểm tên và checksum); ghi id của
   transaction; chạy script; kiểm id của transaction không đổi, tức script không tự kết thúc
   transaction; thêm dòng vào `schema_migrations`; commit.
5. Lỗi ở bất kỳ bước nào thì transaction đó rollback: migration đó và mọi migration sau nó chưa
   được áp, các migration trước đó vẫn giữ.
6. Mỗi migration có một hạn (mặc định 10 phút). Quá hạn thì kết nối bị đóng và server rollback.

## `orion_migrate`

```
orion_migrate [--dir <thư mục>] [--check] [--timeout <giây>]
```

- Chuỗi kết nối libpq lấy từ biến môi trường `ORION_DATABASE`, không từ dòng lệnh: dòng lệnh của
  tiến trình ai trên máy cũng đọc được, mà chuỗi kết nối có thể có mật khẩu.
- `--dir`: thư mục migration, mặc định `db/migrations`.
- `--check`: chỉ kiểm theo bước 1 và 3, không áp gì.
- `--timeout`: hạn của mỗi migration, tính bằng giây.
- Mã thoát: `0` khi DB khớp code (đã áp xong, hay `--check` thấy không còn gì để áp); `1` khi
  `--check` thấy còn migration chưa áp; `2` khi lỗi, kèm lý do trên stderr.

## Test

Mỗi migration trong `db/migrations/` được test áp lên schema rỗng và lên schema của bản trước nó
(CLAUDE.md X.14): `tools/migrate/tests/`, trên PostgreSQL thật, chạy trong CI.
