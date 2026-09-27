# ADR 0010 — Dependency: GoogleTest và Google Benchmark

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.9: thêm dependency cần ADR)
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.1; CLAUDE.md X.4, X.8, X.9; NGHI-NGO-026

## Bối cảnh

ARCH §4.1 chọn GoogleTest cho unit test và Google Benchmark cho benchmark (CLAUDE.md X.4, X.8).
CLAUDE.md X.9 đòi mỗi dependency mới có ADR ghi license, người duy trì và lịch sử CVE, và được ghim
qua baseline của vcpkg. Hai thư viện này chỉ link vào tệp chạy test và benchmark, không vào sản
phẩm.

## Quyết định

| | GoogleTest | Google Benchmark |
|---|---|---|
| Port vcpkg, bản ghim | `gtest` 1.18.0 | `benchmark` 1.9.5 |
| Baseline vcpkg | `07f4812200df3d3c931c0c8a6081d3b21fe2bf9f` | cùng baseline |
| Repo | https://github.com/google/googletest | https://github.com/google/benchmark |
| License | BSD-3-Clause | Apache-2.0 |
| Người duy trì | Google | Google |
| Bản mới nhất | v1.18.0, tag ngày 2026-08-10 | v1.9.5, tag ngày 2026-01-21 |
| Lịch sử CVE | không có CVE nào | không có CVE nào |

1. Cả hai vào `vcpkg.json` như dependency thường; CMake tìm bằng `find_package(... CONFIG)` trong
   `cmake/orion_module.cmake`, chỉ link vào target `*_tests` và `*_bench`.
2. Test biên dịch với exception và RTTI tắt như mọi code khác (X.3); GoogleTest tự tắt phần dùng
   exception khi thấy `-fno-exceptions`.

## Bằng chứng

- License và ngày tag: đọc `LICENSE` và commit của tag trong bản clone git ngày 2026-09-27 —
  googletest `v1.18.0` là commit `063de7e9578f82b369302001269680b4b1553359` (2026-08-10), mở đầu
  bằng "Copyright 2008, Google Inc." và điều khoản BSD 3 mục; benchmark `v1.9.5` là commit
  `192ef10025eb2c4cdd392bc502f0c852196baa48` (2026-01-21), mở đầu bằng "Apache License, Version
  2.0".
- CVE: tra ngày 2026-09-27. nvd.nist.gov và cve.org bị proxy của phiên dựng chặn, nên dữ liệu NVD
  được đọc từ bản sao đầy đủ `fkie-cad/nvd-json-data-feeds` (bản `v2026.09.27-000023`, 398 400 bản
  ghi), cộng trang security của từng repo và tìm kiếm `github.com/advisories`. Không bản ghi nào
  khớp `googletest|gtest|gmock` hay `google benchmark|libbenchmark`; cả hai repo không có security
  advisory nào.

## Hệ quả

- Build cục bộ trong môi trường không tải được nguồn port vcpkg dùng gói hệ điều hành cùng tên CMake
  package; phiên bản khác bản ghim nên chỉ CI là bằng chứng cho bản ghim (NGHI-NGO-026).
- Nâng phiên bản là đổi baseline vcpkg, kèm tra lại CVE, trong cùng commit.

## Phương án đã loại

- Catch2, doctest: ARCH §4.1 đã chọn GoogleTest; đổi cần ADR thay ADR này.
- Tự viết framework test: không có lợi so với chi phí.
