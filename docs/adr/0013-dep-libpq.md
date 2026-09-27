# ADR 0013 — Dependency: libpq

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.9, X.16.2: thêm dependency cần
  ADR)
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.5, §4.6, §4.7, §7, §8; CLAUDE.md X.2, X.5, X.9, X.14; NGHI-NGO-026,
  NGHI-NGO-035

## Bối cảnh

ARCH §4.5 và §4.6 chọn libpq, API chính thức của PostgreSQL, cho mọi server nói chuyện với DB, bọc
trong `game/server/lib/db`, truy vấn có tham số. CLAUDE.md X.9 đòi mỗi dependency mới có ADR ghi
license, người duy trì và lịch sử CVE, và được ghim qua baseline của vcpkg. libpq chỉ sống ở server:
client, bot và bản mobile không bao giờ link nó.

## Quyết định

| | libpq |
|---|---|
| Port vcpkg, bản ghim | `libpq` 18.4, không bật tính năng mặc định |
| Baseline vcpkg | `07f4812200df3d3c931c0c8a6081d3b21fe2bf9f` |
| Repo | https://git.postgresql.org (bản sao chính thức: https://github.com/postgres/postgres), thư mục `src/interfaces/libpq` |
| License | PostgreSQL |
| Người duy trì | PostgreSQL Global Development Group; PostgreSQL tự là CNA cấp CVE |
| Bản mới nhất | 18.6 (2026-08-13); 18.5 không bao giờ được phát hành |
| Lịch sử CVE | 7 CVE của libpq từ 2021-09 tới 2026-09; không cái nào ảnh hưởng 18.4 |

1. `vcpkg.json` có `{"name": "libpq", "default-features": false, "platform": "linux | windows"}`.
   Tính năng mặc định của port (`openssl`, `zlib`, `lz4`) kéo thêm ba dependency mà code không dùng
   tới, trừ TLS tới PostgreSQL; bật TLS là một quyết định riêng (mục Hệ quả). Server chạy trên
   Linux và được build trên Windows để debug trên máy dev (ARCH §4.5), nên `game/server/` chỉ được
   build cho hai hệ đó (`game/CMakeLists.txt`), và Android, iOS, macOS không cài libpq.
2. CMake tìm bằng `find_package(PostgreSQL REQUIRED)`, target `PostgreSQL::PostgreSQL` (cách dùng
   của port vcpkg, cũng là module `FindPostgreSQL` của CMake), link PRIVATE vào `server_db`. Wrapper
   của port thêm thư viện phụ của bản tĩnh: `pgcommon`, `pgport`, và `secur32`, `wldap32` trên
   Windows; `server_db` tự link `ws2_32`.
3. Chỉ `game/server/lib/db` include `<libpq-fe.h>` và `<postgres_ext.h>` (CLAUDE.md X.2;
   `tools/check_layers.py` gán cả hai cho module này). Header public của `server_db` không lộ kiểu
   nào của libpq.
4. Mọi lời gọi có hạn (X.14), nên chỉ dùng API bất đồng bộ trên kết nối không chặn, và chờ socket
   bằng `poll` (Linux) hay `WSAPoll` (Windows) không quá mốc hạn của bên gọi:
   - kết nối: `PQconninfoParse`, `PQconninfoFree`, `PQfreemem`, `PQconnectStartParams`,
     `PQconnectPoll`, `PQstatus`, `PQsocket`, `PQsetnonblocking`, `PQserverVersion`,
     `PQsetNoticeReceiver`, `PQerrorMessage`, `PQtransactionStatus`, `PQfinish`;
   - câu lệnh: `PQsendQueryParams` (tham số và kết quả nhị phân), `PQsendQuery` (script migration),
     `PQflush`, `PQconsumeInput`, `PQisBusy`, `PQgetResult`;
   - kết quả: `PQresultStatus`, `PQresultErrorField`, `PQresultErrorMessage`, `PQntuples`,
     `PQnfields`, `PQftype`, `PQgetvalue`, `PQgetlength`, `PQgetisnull`, `PQcmdTuples`,
     `PQcmdStatus`, `PQclear`.

   Không dùng các hàm escape (`PQescapeLiteral`, `PQescapeString`, ...: chỗ của CVE-2025-1094) và
   không dùng large object (`lo_*`, `PQfn`: chỗ của CVE-2026-6477): giá trị luôn đi qua tham số.
   Không dùng `PQcancel`: huỷ truy vấn phải mở thêm một kết nối, việc cũng có thể treo quá hạn; quá
   hạn thì kết nối bị đóng, và server tự huỷ truy vấn nhờ `client_connection_check_interval`.

## Bằng chứng

- License: tệp `COPYRIGHT` ở tag `REL_18_4` (bản sao `github.com/postgres/postgres`): "Portions
  Copyright (c) 1996-2026, PostgreSQL Global Development Group; Portions Copyright (c) 1994, The
  Regents of the University of California", rồi đoạn cho phép của license PostgreSQL.
- Bảo trì: 18.4 phát hành 2026-05-14, 18.6 ngày 2026-08-13; ghi chú phát hành nói 18.5 không được
  phát hành vì lỗi hồi quy phát hiện sau khi đóng gói (tag `REL_18_4`, `REL_18_6`, không có
  `REL_18_5`).
- CVE, tra ngày 2026-09-27 trên bản sao NVD `fkie-cad/nvd-json-data-feeds` bản `v2026.09.27-000023`
  (nvd.nist.gov, cve.org và postgresql.org bị proxy của phiên dựng chặn), bản ghi CVE chính thức
  `CVEProject/cvelistV5`, `github.com/advisories?query=libpq`, và ghi chú phát hành
  `release-14/15/17/18.sgml`. Các CVE của chính libpq trong 5 năm, kèm bản sửa:
  - CVE-2021-23222 (dữ liệu không mã hoá sau bắt tay SSL/GSS): 14.1 và các nhánh cũ;
  - CVE-2022-41862 (đọc quá ở GSSAPI): 15.2 và các nhánh cũ;
  - CVE-2024-10977 (thông điệp lỗi của server trong lúc thương lượng SSL/GSS): 17.1 và các nhánh
    cũ;
  - CVE-2025-1094 (các hàm escape với mã hoá sai, SQL injection qua psql): 17.3 và các nhánh cũ;
  - CVE-2025-4207 (đọc quá một byte khi kiểm GB18030): 17.5 và các nhánh cũ;
  - CVE-2025-12818 (tràn số khi tính cỡ cấp phát): 18.1 và các nhánh cũ;
  - CVE-2026-6477 (`PQfn` ghi đè stack của client): 18.4 và các nhánh cũ.

  Bản ghim 18.4 nằm ngoài khoảng ảnh hưởng của cả bảy. CVE-2026-6473 và CVE-2026-6478 được Red Hat
  ghi cho gói libpq độc lập nhưng cũng "trước 18.4". 18.6 sửa thêm lỗi không có CVE trong libpq và
  CVE trong các chương trình client (psql, pg_dump, ECPG), mà port chỉ build khi bật tính năng
  `client`.
- Công thức port: `ports/libpq/vcpkg.json` (`"supports": "!uwp & !emscripten"`, build bằng meson,
  cần bison, flex và perl của máy build), `portfile.cmake`, `usage` và `vcpkg-cmake-wrapper.cmake`
  ở baseline trên.

## Hệ quả

- Build cục bộ trong container không tải được nguồn port vcpkg dùng gói Ubuntu `libpq-dev` 16.15 qua
  `FindPostgreSQL`; chỉ CI là bằng chứng cho bản ghim (NGHI-NGO-026). Mọi hàm ở mục 4 đều có từ
  libpq 10; `client_connection_check_interval` của server cần PostgreSQL 14 trở lên, và `server_db`
  từ chối server cũ hơn. Test của `server_db` chạy trên PostgreSQL thật: PostgreSQL 16 trong
  container dựng, PostgreSQL 18.6 trong các job Linux của CI (NGHI-NGO-035).
- Job Linux của CI cài thêm `bison` và `flex` cho port.
- Kết nối tới DB không có TLS: port không bật `openssl`. ARCH §4.7 đặt DB trong mạng riêng của cụm;
  cần TLS (ví dụ DB dịch vụ đám mây) thì bật tính năng `openssl` là một ADR mới, kèm lịch sử CVE của
  OpenSSL. Xác thực SCRAM-SHA-256 vẫn dùng được: libpq tự có SHA-256 và lấy số ngẫu nhiên của hệ
  điều hành khi không có OpenSSL (CI kiểm bằng mật khẩu, NGHI-NGO-035).
- Tra DNS của tên máy trong chuỗi kết nối là đồng bộ bên trong libpq, không theo hạn; cấu hình
  production dùng `hostaddr`.
- Nâng phiên bản là đổi baseline vcpkg, kèm tra lại CVE, trong cùng commit. 18.6 đã có: nâng khi
  baseline mới chứa nó.

## Phương án đã loại

- libpqxx: thêm một dependency C++ dùng exception, trong khi dự án build không exception (X.3).
- Driver tự viết nói giao thức PostgreSQL: một parser dữ liệu từ ngoài nữa phải tự bảo trì và fuzz,
  không lợi gì so với thư viện chính thức.
- Bật tính năng mặc định của port: thêm OpenSSL, zlib, lz4 vào sản phẩm khi chưa ai cần TLS tới DB.
- `PQexecParams` (API chặn): không đặt được hạn cho một lời gọi; khi mạng treo, luồng gọi chờ tới
  khi TCP bỏ cuộc, trái X.14.
