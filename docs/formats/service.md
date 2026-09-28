# Tiến trình server: dòng lệnh, cấu hình, bí mật, log, dừng

Mọi tiến trình server (`orion_<tên>` ở ARCH §1.2) khởi động và dừng theo cùng một cách, qua
`game/server/lib/service`. Tài liệu này là hợp đồng giữa các tiến trình đó và bên chạy chúng:
`tools/run/cluster.py` ở máy dev, cấu hình trong `deploy/`, người vận hành.

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này và code trong cùng commit (CLAUDE.md
X.15). Mục riêng của từng tiến trình (ví dụ mục `auth` của `orion_auth`) nằm trong tài liệu của
tiến trình đó.

## Thứ tự trong main

1. `StopSignal::install()` — trước khi tạo luồng nào (mục "Dừng").
2. `parse_arguments`, `ConfigFile::load`, `read_common`, rồi mục riêng của tiến trình.
3. `LogRuntime::start` với `core::JsonLinesSink` ra stdout, trước mọi luồng khác (mục "Log").
4. Phần riêng của tiến trình: nạp khoá, mở DB, bật server HTTP, …
5. `StopSignal::wait()`, rồi dừng theo thứ tự ngược lại; `LogRuntime` bị huỷ sau cùng.

Lỗi ở bước 2 hay 4 (cấu hình sai, khoá sai, không mở được cổng) thì tiến trình không chạy: cấu hình
và khoá là dữ liệu từ ngoài, nên sai là lỗi có ngữ cảnh nêu tên trường, không bao giờ là assert
(CLAUDE.md X.5).

## Dòng lệnh

```
orion_<tên> --config <tệp>
```

Đúng một cặp `--config <tệp>`; thiếu, thừa hay lạ là `InvalidArgument`. Đường dẫn khác rỗng và chỉ
gồm ký tự ASCII: `argv` trên Windows theo code page ANSI chứ không phải UTF-8 mà `engine/io` đòi, nên
chỉ ASCII mới đọc giống nhau trên hai nền tảng.

## Cấu hình

Tệp cấu hình là JSON theo các luật đọc của `docs/formats/http.md` (mục "JSON": UTF-8, không khoá
trùng, lồng tối đa 32 cấp, …), tối đa 1 MiB, gốc là object. Trường lạ ở bất kỳ đâu là lỗi, để gõ
sai tên trường không bị bỏ qua lặng lẽ; chỉ tên mục riêng mà tiến trình khai mới được thêm ở gốc.

Đường dẫn trong cấu hình là UTF-8. Đường dẫn tương đối tính từ thư mục chứa tệp cấu hình. Đường dẫn
bắt đầu bằng `/`, `\` hay `<chữ cái>:` là tuyệt đối, theo cùng một luật trên mọi nền tảng.

### Gốc

| Trường | Kiểu | Mặc định | Ý nghĩa |
|---|---|---|---|
| `environment` | `"local"` hay `"production"` | bắt buộc | môi trường chạy; quyết định bí mật dev có được nạp không |
| `log` | object | `{"level":"info"}` | mục "Log" |
| `http` | object | không có | server HTTP của tiến trình, nếu nó có |
| `database` | object | không có | kết nối PostgreSQL, nếu tiến trình dùng |

### `log`

| Trường | Kiểu | Mặc định |
|---|---|---|
| `level` | `"trace"`, `"debug"`, `"info"`, `"warn"`, `"error"` | `"info"` |

### `http`

Mỗi trường là một trường của `http::ServerConfig` hay `http::Limits` (`docs/formats/http.md`, mục
"Giới hạn và hạn"); mặc định là mặc định của hai kiểu đó.

| Trường | Khoảng | Mặc định |
|---|---|---|
| `address` | IPv4 hay IPv6 dạng số, tối đa 64 byte | `"127.0.0.1"` |
| `port` | 1..65535 | bắt buộc |
| `io_threads` | 1..64 | 1 |
| `workers` | 1..256 | 4 |
| `queue_capacity` | 1..65536 | 64 |
| `max_connections` | 1..65536 | 1024 |
| `max_header_bytes` | 256..65535 | 8192 |
| `max_body_bytes` | 0..16777216 | 65536 |
| `max_requests_per_connection` | 1..1000000 | 1000 |
| `read_timeout_ms` | 1..600000 | 10000 |
| `idle_timeout_ms` | 1..600000 | 75000 |
| `write_timeout_ms` | 1..600000 | 10000 |
| `linger_timeout_ms` | 1..600000 | 2000 |
| `handler_timeout_ms` | 1..600000 | 10000 |
| `check_interval_ms` | 1..10000 | 100 |

`address` có đúng dạng số không thì server HTTP kiểm khi mở cổng. Dịch vụ HTTP chỉ nghe sau load
balancer (CLAUDE.md X.14): `0.0.0.0` chỉ dùng khi mạng của máy chạy đã chặn cổng này từ internet.

### `database`

| Trường | Khoảng | Mặc định |
|---|---|---|
| `conninfo_file` | đường dẫn tới tệp bí mật chứa chuỗi kết nối libpq, tối đa 1024 byte | bắt buộc |
| `connect_timeout_ms` | 1..600000 | 5000 |
| `statement_timeout_ms` | 0..600000; 0 là không giới hạn | 30000 |
| `client_check_interval_ms` | 0..600000; 0 là tắt | 1000 |

Chuỗi kết nối nằm trong tệp riêng, không trong tệp cấu hình, vì nó chứa mật khẩu (mục "Khoá và bí
mật"). `statement_timeout_ms` và `client_check_interval_ms` là `db::Options` (`game/server/lib/db`).

## Khoá và bí mật

Tệp bí mật: tối đa 4096 byte; một dòng mới ở cuối (`\n` hay `\r\n`) được bỏ; phần còn lại không được
rỗng. Tệp khoá là tệp bí mật chứa đúng 64 chữ số hex (hoa hay thường), tức 32 byte:

- khoá ký: seed Ed25519, từ đó dựng cặp khoá (`crypto::signing_key_pair_from_seed`);
- khoá công khai: 32 byte khoá công khai Ed25519.

Sai định dạng là `InvalidArgument`; lớn hơn 4096 byte là `OutOfRange`.

Tệp có `.dev.` trong tên (phần sau dấu `/` hay `\` cuối) là bí mật dev: khoá giả trong
`deploy/local/` (ARCH §2), chỉ để chạy cụm trên máy dev. Nó chỉ được nạp khi `environment` là
`"local"`, và bản ship (`ORION_SHIP`) từ chối nó ở mọi môi trường (CLAUDE.md X.9); ngoài hai trường
hợp đó là `FailedPrecondition`, nên tiến trình không khởi động.

Bí mật không nằm lâu hơn cần thiết trong bộ nhớ: văn bản hex của khoá bị ghi 0 ngay sau khi giải mã,
chuỗi kết nối bị ghi 0 khi `DatabaseConfig` bị huỷ hay bị move đi. Không log bí mật (CLAUDE.md X.5).

## Log

Log là JSON lines ra stdout (`core::format_json_line`, CLAUDE.md X.5); bên chạy tiến trình chuyển
stdout vào tệp (máy dev: mỗi tiến trình một tệp trong `out/run/logs/`, ARCH §1.2). Một luồng ghi riêng
(`log-writer`, ADR 0007) ghi ra; luồng khác chỉ đẩy bản ghi vào hàng đợi có giới hạn, đầy thì bỏ và
báo số bị bỏ.

`LogRuntime` đặt logger toàn cục trước khi tạo luồng ghi và gỡ nó trước khi dừng: log gọi sau đó
bị bỏ. Huỷ `LogRuntime` ghi nốt mọi bản ghi đã nhận.

## Dừng

Tiến trình dừng êm khi nhận:

- Linux: SIGINT hay SIGTERM (`systemctl stop`, `docker stop`, Ctrl+C);
- Windows: Ctrl+C hay Ctrl+Break; đóng console; với tiến trình chạy như dịch vụ, đăng xuất và tắt
  máy.

Linux: `StopSignal::install` chặn SIGINT, SIGTERM ở luồng gọi, nên mọi luồng tạo sau thừa hưởng việc
chặn. Hai tín hiệu không bao giờ chạy handler bất đồng bộ; `wait` đọc chúng qua signalfd. SIGPIPE bị
bỏ qua cho cả tiến trình: ghi vào socket hay pipe đã đóng (stdout khi bên đọc log đã thoát) là lỗi
EPIPE cho bên ghi xử lý, không kết thúc tiến trình.

Windows: handler console báo một event có tên theo id tiến trình (`Local\orion-stop-<pid>`), mà
`wait` chờ. Theo tài liệu HandlerRoutine của Microsoft, hệ điều hành gọi handler trên một luồng mới
nó tạo trong tiến trình. Với đóng console, đăng xuất, tắt máy, hệ điều hành kết thúc tiến trình khi
handler trả về, hay khi hết thời gian chờ (5 giây với đóng console), nên handler giữ luồng đó tới
khi main dừng xong và tiến trình kết thúc; chưa đo với sự kiện console thật (NGHI-NGO-036).
Ctrl+C, Ctrl+Break không có thời gian chờ. Tên event đã có (tiến trình khác tạo trước) thì `install`
báo `AlreadyExists` và tiến trình không khởi động.

`StopSignal::request_stop` làm `wait` trả về như khi có tín hiệu dừng: cho lệnh dừng của chính
tiến trình và cho test. Một khi đã có yêu cầu dừng thì mọi lần `wait` sau trả về ngay.
