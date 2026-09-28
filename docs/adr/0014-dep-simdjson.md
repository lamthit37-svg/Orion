# ADR 0014 — Dependency: simdjson

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.9, X.16.2: thêm dependency cần
  ADR)
- **Ngày:** 2026-09-28
- **Liên quan:** ARCH §4.6, §9; CLAUDE.md X.2, X.3, X.4, X.5, X.9, X.14; NGHI-NGO-003, NGHI-NGO-026

## Bối cảnh

ARCH §4.6 chọn simdjson để đọc body JSON của các dịch vụ HTTP (auth, account, store, patch, admin),
qua API mã lỗi, và một writer nhỏ tự viết để ghi. CLAUDE.md X.14 đòi mỗi endpoint có một parser
request có kiểu và mỗi parser có một fuzz target; X.3 đòi code của `game/` build không exception.
CLAUDE.md X.9 đòi mỗi dependency mới có ADR ghi license, người duy trì và lịch sử CVE, và được ghim
qua baseline của vcpkg. simdjson chỉ sống ở server: client, bot và bản mobile không link nó.

## Quyết định

| | simdjson |
|---|---|
| Port vcpkg, bản ghim | `simdjson` 4.6.11, tắt tính năng mặc định, chỉ bật `utf8-validation` |
| Baseline vcpkg | `07f4812200df3d3c931c0c8a6081d3b21fe2bf9f` |
| Repo | https://github.com/simdjson/simdjson |
| License | Apache-2.0 hoặc MIT, tuỳ chọn; kèm mã BSL-1.0 và BSD-3-Clause (biểu thức của port: `(Apache-2.0 OR MIT) AND BSL-1.0 AND BSD-3-Clause`) |
| Người duy trì | dự án simdjson (tổ chức GitHub `simdjson`), dẫn dắt bởi Daniel Lemire |
| Bản mới nhất | v4.6.11 (2026-09-05), trùng bản ghim |
| Lịch sử CVE | 2 CVE, cả hai năm 2026; không cái nào ảnh hưởng 4.6.11 |

1. `vcpkg.json` có `{"name": "simdjson", "default-features": false, "features":
   ["utf8-validation"], "platform": "linux | windows"}`:
   - bỏ `exceptions`: code của `game/` build không exception (X.3), và thư viện phải build cùng cách
     để hàm inline trong header giống nhau ở cả hai phía;
   - bỏ `threads`: không dùng `parse_many` đa luồng;
   - bỏ `deprecated`: không dùng API cũ;
   - giữ `utf8-validation`: body là dữ liệu từ ngoài, chuỗi đọc ra phải là UTF-8 hợp lệ (X.3).

   Server chỉ build cho Linux và Windows (`game/CMakeLists.txt`), như libpq (ADR 0013).
2. CMake tìm bằng `find_package(simdjson CONFIG REQUIRED)`, target `simdjson::simdjson`, link PRIVATE
   vào `server_http`. Chỉ `game/server/lib/http` include `<simdjson.h>` (CLAUDE.md X.2;
   `tools/check_layers.py` gán header này cho module đó). Header public (`json.hpp`) không lộ kiểu
   nào của simdjson: `Document` chép cây của simdjson sang cây của chính nó.
3. Chỉ dùng API DOM với mã lỗi: `dom::parser` (`allocate` với độ sâu tối đa, `parse` với
   `realloc_if_needed`), `dom::element` (`type`, `get_int64`, `get_uint64`, `get_double`,
   `get_string`, `get_bool`, `get_array`, `get_object`), iterator của `dom::array` và `dom::object`.
   Không dùng API ondemand, không dùng `parse_unpadded` (chỗ của CVE-2026-88358), không dùng
   document builder (`string_builder`, chỗ của CVE-2026-8295).
4. `server_http` thêm những luật simdjson không có: object có khoá trùng bị từ chối, độ sâu tối đa
   32, tối đa 65 536 giá trị và 1 MiB mỗi tài liệu (docs/formats/http.md, mục "JSON").

## Bằng chứng

- License: `LICENSE` (Apache 2.0) và `LICENSE-MIT` ở tag `v4.6.11`; README: "available under the
  Apache License 2.0 as well as under the MIT License. As a user, you can pick the license you
  prefer."
- Bảo trì: tag `v4.6.11` ngày 2026-09-05 trên nhánh `4.6.x`; 78 commit trong 90 ngày tới 2026-09-25.
- CVE, tra ngày 2026-09-27 trên bản sao NVD `fkie-cad/nvd-json-data-feeds` bản `v2026.09.27-000023`,
  bản ghi CVE chính thức `CVEProject/cvelistV5` và `github.com/advisories?query=simdjson`:
  - CVE-2026-8295: tràn số nguyên trong `string_builder::escape_and_append()` của document builder
    trên bản `size_t` hẹp (32 bit), dẫn tới đọc ngoài biên. Ảnh hưởng < 4.6.4; sửa ở 4.6.4 (commit
    `f01641e`, là tổ tiên của `v4.6.11`).
  - CVE-2026-88358: đọc quá một byte trong `dom::parser::parse_unpadded()` với tài liệu lồng bị cắt.
    Hàm này chỉ có trên nhánh `master` (thêm ở commit `172f0b2`), không có trong tag `v4.6.1` hay
    `v4.6.11`: bản ghim không có mã bị ảnh hưởng (suy ra từ mã nguồn; bản ghi CVE không nêu khoảng
    phiên bản).

  CVE-2019-15550 là của crate Rust `simd-json`, NVD gán nhầm CPE `simdjson_project:simdjson`
  (> 0.1.13, < 0.1.15); máy quét có thể báo nó, 4.6.11 nằm ngoài khoảng đó.
- Fuzz liên tục: simdjson có trong OSS-Fuzz (`google/oss-fuzz`, `projects/simdjson/project.yaml`,
  sanitizer address và undefined).
- Công thức port: `ports/simdjson/vcpkg.json` (tính năng mặc định `deprecated`, `exceptions`,
  `threads`, `utf8-validation`) và `portfile.cmake` (`exceptions` → `SIMDJSON_EXCEPTIONS`,
  `utf8-validation` đảo thành `SIMDJSON_SKIPUTF8VALIDATION`) ở baseline trên.

## Hệ quả

- Build cục bộ trong container dựng không dùng được `libsimdjson-dev` 3.6.4 của Ubuntu: header của
  nó không biên dịch với clang 20 (`no member named 'print_newline' in 'base_formatter<formatter>'`;
  clang từ bản 19 kiểm thành viên của current instantiation ngay khi định nghĩa template). Build cục
  bộ vì vậy dùng hai tệp single-header của đúng tag `v4.6.11` lấy từ repo chính thức, build tĩnh
  không exception trong một prefix riêng của máy dựng (không vào repo); CI dùng port vcpkg
  (NGHI-NGO-026).
- Hành vi đã đo trên 4.6.11 và được test giữ: BOM UTF-8 đầu tài liệu được bỏ qua (RFC 8259 cho
  phép); số nguyên vượt 64 bit làm hỏng cả tài liệu; `-0` là số nguyên 0 còn `-0.0` là số thực;
  `\u0000` hợp lệ trong chuỗi; surrogate lẻ và ký tự điều khiển chưa escape là lỗi.
- Nâng phiên bản là đổi baseline vcpkg, kèm tra lại CVE và chạy lại fuzz target `json_document`,
  trong cùng commit.

## Phương án đã loại

- nlohmann/json, RapidJSON, Boost.JSON: đều có đường ném exception trong API thường dùng hay cần
  bọc thêm để tắt; simdjson đã được ARCH §4.6 chọn và có API mã lỗi đầy đủ cho DOM.
- API ondemand của simdjson: đọc lười, lỗi cú pháp ở phần chưa đọc có thể không bao giờ lộ ra, trong
  khi parser request cần kiểm cả body trước khi dùng bất kỳ trường nào (kiểm rồi mới áp, X.10).
- Parser JSON tự viết: thêm một parser dữ liệu từ ngoài phải tự bảo trì và fuzz, không lợi gì so
  với một thư viện được fuzz liên tục ở OSS-Fuzz.
