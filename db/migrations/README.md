# Migration của PostgreSQL

Mỗi tệp `NNNN_<tên>.sql` ở đây là một bước đổi schema, áp bằng `orion_migrate`, mỗi tệp một
transaction (CLAUDE.md X.14). Luật đầy đủ: [docs/formats/migrations.md](../../docs/formats/migrations.md).

- Thêm tệp mới với phiên bản kế tiếp. Không sửa, không đổi tên tệp đã merge: checksum trong bảng
  `schema_migrations` bắt điều đó.
- Không `BEGIN`, `COMMIT`, `ROLLBACK` trong tệp; tên bảng không kèm schema.
- Test của `tools/migrate` áp mọi tệp ở đây lên schema rỗng, và từng tệp lên schema của bản trước nó.
- Chạy tay: đặt `ORION_DATABASE` là chuỗi kết nối libpq, rồi chạy `orion_migrate` từ gốc repo
  (`--check` để chỉ kiểm).
