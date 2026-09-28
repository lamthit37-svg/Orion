# ADR 0015 — Dependency: Boost.Beast và Boost.Asio

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.9, X.16.2: thêm dependency cần
  ADR)
- **Ngày:** 2026-09-28
- **Liên quan:** ARCH §4.6, §7, §9; ADR 0007; CLAUDE.md X.2, X.3, X.7, X.9, X.14; NGHI-NGO-002,
  NGHI-NGO-026

## Bối cảnh

ARCH §4.6 chọn Boost.Beast trên Boost.Asio cho HTTP/1.1 của các dịch vụ HTTP, bọc trong
`game/server/lib/http`, đứng sau load balancer; ARCH §7 đặt các dịch vụ này trên một nhóm luồng IO
cố định của Asio. CLAUDE.md X.3 đòi code của `game/` build không exception và không RTTI, hoặc bọc
thư viện cần exception trong một module riêng bắt hết ở ranh giới (NGHI-NGO-002). CLAUDE.md X.9 đòi
mỗi dependency mới có ADR ghi license, người duy trì và lịch sử CVE, và được ghim qua baseline của
vcpkg. Beast và Asio chỉ sống ở server.

## Quyết định

| | Boost.Beast, Boost.Asio |
|---|---|
| Port vcpkg, bản ghim | `boost-beast` 1.92.0 (kéo theo `boost-asio` 1.92.0 và các port Boost nó cần) |
| Baseline vcpkg | `07f4812200df3d3c931c0c8a6081d3b21fe2bf9f` |
| Repo | https://github.com/boostorg/beast, https://github.com/boostorg/asio (bản đóng gói Boost của Asio, gốc ở https://github.com/chriskohlhoff/asio) |
| License | BSL-1.0 |
| Người duy trì | Beast: Vinnie Falco, Mohammad Nejati; Asio: Chris Kohlhoff; cả hai thuộc Boost C++ Libraries |
| Bản mới nhất | Boost 1.92.0 (2026-08-05), trùng bản ghim; Beast phiên bản 361 |
| Lịch sử CVE | 3 CVE và 1 advisory GitHub không CVE; không cái nào ảnh hưởng 1.92.0 |

1. `vcpkg.json` có `{"name": "boost-beast", "platform": "linux | windows"}`. Port `boost-beast`
   đòi `boost-asio` với tính năng mặc định (`deadline-timer`, `spawn`), nên chúng được build dù
   code không dùng. Server chỉ build cho Linux và Windows, như libpq (ADR 0013).
2. Beast và Asio là thư viện chỉ header; CMake tìm bằng `find_package(Boost 1.83 REQUIRED)` và link
   PRIVATE `Boost::headers` vào `server_http`, cộng `ws2_32`, `mswsock` và
   `_WIN32_WINNT=0x0A00` (Windows 10) trên Windows. Chỉ `game/server/lib/http` include
   `<boost/...>` (CLAUDE.md X.2; `tools/check_layers.py` gán tiền tố `boost/` cho module đó); test
   của module dùng Asio làm client qua loopback. Header public của `server_http` không lộ kiểu nào
   của Boost. `.clang-tidy` cho `misc-include-cleaner` bỏ qua header của Boost: header public của
   Boost chỉ chuyển tiếp tới `impl/`, `detail/`, và Boost không có pragma IWYU, nên check đó đòi
   include thẳng các tệp nội bộ.
3. Build không exception, không RTTI, như mọi code của `game/`: Boost tự nhận ra (`BOOST_NO_EXCEPTIONS`,
   `BOOST_NO_RTTI`). Khi đó Boost gọi `boost::throw_exception` mà bên dùng phải định nghĩa;
   `server_http` định nghĩa nó bằng `ORION_VERIFY`: crash kèm minidump (CLAUDE.md X.5). Code chỉ
   gọi các overload nhận `error_code`, nên đường ném chỉ còn là lỗi lập trình hay hết tài nguyên lúc
   khởi động.
4. Chỉ dùng: `asio::io_context`, `strand`, `executor_work_guard`, `ip::tcp::acceptor`,
   `ip::tcp::socket`, `steady_timer` (chỉ để kích việc quét hạn), `post`; `beast::flat_buffer`,
   `http::request_parser<string_body>` (`header_limit`, `body_limit`, `put`),
   `http::async_read_header`, `http::async_read`, `http::async_write`,
   `http::response<string_body>`. Không dùng `beast::tcp_stream`: hạn của nó chạy trên đồng hồ
   thật, còn server quét hạn trên đồng hồ tiêm được (CLAUDE.md X.4). Không dùng WebSocket, SSL,
   zlib của Beast (chỗ của CVE-2016-9840 và CVE-2018-25032), coroutine, `spawn`, hay `operator<<`
   của message (hệ quả, bên dưới).
5. Request có `Transfer-Encoding` bị từ chối bằng 411 (docs/formats/http.md): server không bao giờ
   đọc body chunked hay trailer, nên chỗ của GHSA-8c6g-xf48-2fvx không tới được, kể cả với bản Beast
   cũ hơn 1.90 của build cục bộ.

## Bằng chứng

- License: `LICENSE_1_0.txt` (Boost Software License 1.0) ở tag `boost-1.92.0` của Beast và của
  superproject; header của boostorg/asio ghi "Distributed under the Boost Software License, Version
  1.0".
- Người duy trì: `meta/libraries.json` ở tag `boost-1.92.0` của Beast và Asio.
- Bảo trì: tag `boost-1.92.0` của `boostorg/boost` (commit 2026-08-05); 1.91.0 ngày 2026-04-15;
  nhánh `develop` của Beast có commit ngày 2026-09-25; boostorg/asio có 44 commit trong 90 ngày.
- CVE, tra ngày 2026-09-27 trên bản sao NVD `fkie-cad/nvd-json-data-feeds` bản `v2026.09.27-000023`,
  bản ghi CVE chính thức `CVEProject/cvelistV5`, trang security của các repo và
  `github.com/advisories`:
  - CVE-2019-25219 (Asio không có mã lỗi dự phòng khi OpenSSL báo `SSL_ERROR_SYSCALL` không kèm
    lỗi): Asio < 1.13.0, tức Boost ≤ 1.69; sửa ở Boost 1.70.0.
  - CVE-2016-9840 (zlib `inftrees.c`, trong bản port zlib của Beast): NVD gán `boost:boost` < 1.78.0;
    sửa ở Boost 1.78.0 (Beast 321).
  - CVE-2018-25032 (zlib hỏng bộ nhớ khi nén): NVD không gán Boost; CHANGELOG của Beast sửa ở phiên
    bản 331 (Boost 1.81.0).
  - GHSA-8c6g-xf48-2fvx (không CVE; 2026-05-19; CVSS 4.0 6.3): header smuggling, Beast gộp trailer
    của body chunked vào header của request; "<= 1.89.0". Boost 1.90.0 (Beast 359) cho
    `http::parser` từ chối trailer không chuẩn theo mặc định, Boost 1.92.0 (Beast 361) còn bỏ các
    trường framing và connection trong trailer; hai commit đó là bản sửa là suy luận, advisory không
    nêu bản đã vá.

  Mọi commit sửa ở trên là tổ tiên của `boost-1.92.0` (kiểm bằng git). CVE-2026-11460 là của
  Boost.Serialization, không phải dependency của port `boost-beast`.
- Fuzz liên tục: Beast có trong OSS-Fuzz (`google/oss-fuzz`, `projects/boost-beast/project.yaml`,
  engine libFuzzer, AFL và honggfuzz).
- Công thức port: `ports/boost-beast/vcpkg.json` và `ports/boost-asio/vcpkg.json` ở baseline trên;
  `ports/boost-cmake/usage`: "find_package(Boost REQUIRED [COMPONENTS <libs>...])".

## Hệ quả

- Build cục bộ trong container dựng dùng `libboost1.83-dev` của Ubuntu (Beast 1.83), vì container
  không tải được nguồn port vcpkg (NGHI-NGO-026). Bản đó nằm trong khoảng của GHSA-8c6g-xf48-2fvx;
  quyết định 5 làm server không chạm tới chỗ lỗi. Chỉ CI dùng bản ghim 1.92.0.
- Đã đo cục bộ (Boost 1.83, clang 20): Beast và Asio biên dịch, link và chạy với
  `-fno-exceptions -fno-rtti` và toàn bộ cờ cảnh báo của dự án khi header Boost được include dạng
  SYSTEM (NGHI-NGO-002).
- Hành vi của Beast 1.83 đã đo, và `server_http` được viết theo đó (test giữ):
  - chỉ đọc được dòng request của HTTP/1.0 và 1.1; phiên bản khác là `bad_version`, nên server trả
    400 (HTTP/1.0 thì server trả 505);
  - `header_limit` áp riêng cho dòng request và cho phần header, nên server tự kiểm tổng;
  - Content-Length lớn hơn `body_limit` bị từ chối ngay khi đọc xong header, trước khi đọc body;
  - obs-fold được thay bằng khoảng trắng (RFC 9112 §5.2 cho phép);
  - `prepare_payload` đặt `Content-Length: 0` cho 204, trái RFC 9110 §8.6, nên server không gọi
    nó cho 204;
  - `basic_fields` ném exception khi tên hay giá trị header dài từ 65 534 byte, `flat_buffer`
    ném khi vượt sức chứa, `prepare_payload` ném khi 204 có body: server giới hạn
    `max_header_bytes` ở 65 535, header của handler ở 8 KiB, và tự kiểm trước khi gọi;
  - `socket::cancel` chỉ huỷ thao tác đang chờ trong reactor; thao tác tổng hợp của Beast bắt đầu
    lượt kế tiếp ngay sau một lượt vừa xong thì không bị huỷ. Server vì vậy đóng socket (đang
    chờ, ghi, đóng êm) hay đóng chiều nhận (đang đọc, để còn trả 408) khi quá hạn;
  - `operator<<` của message đi qua `std::aligned_storage`, mà libstdc++ 14 đánh dấu deprecated ở
    C++23 (cảnh báo thành lỗi): không dùng.
- clang-tidy thấy vòng gọi tĩnh qua thao tác tổng hợp của Beast (`misc-no-recursion`); lúc chạy
  không có đệ quy vì Asio không gọi hàm nhận kết quả từ trong hàm bắt đầu thao tác. Chỗ đó có
  NOLINT kèm lý do trong `detail/connection.cpp`.
- Build của Beast chậm (header nặng): chỉ `server_http` include nó, mọi dịch vụ gọi qua API của
  module.
- Nâng phiên bản là đổi baseline vcpkg, kèm tra lại CVE, advisory và chạy lại fuzz target
  `http_request`, trong cùng commit.

## Phương án đã loại

- Thư viện HTTP khác (cpp-httplib, Drogon, Crow): ARCH §4.6 đã chọn Beast; chưa có lý do đo được
  để mở lại quyết định đó.
- Asio bản độc lập (không Boost): cùng mã, nhưng Beast cần Boost.Asio.
- Server HTTP tự viết trên socket: thêm một parser HTTP của dữ liệu từ ngoài phải tự bảo trì, trong
  khi parser của Beast đã có giới hạn header, body và được fuzz ở OSS-Fuzz.
