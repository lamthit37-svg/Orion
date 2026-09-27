# ADR 0012 — Dependency: Zstandard (zstd)

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.9, X.16.2: thêm dependency cần
  ADR)
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.1, §4.4; CLAUDE.md X.5, X.9; NGHI-NGO-026

## Bối cảnh

ARCH §4.1 chọn Zstd để nén pak và patch, với dictionary cho file nhỏ; ARCH §4.4 dùng nó cho pak
của cooker. `engine/io` giải nén từng entry của pak khi đọc. CLAUDE.md X.9 đòi mỗi dependency mới có
ADR ghi license, người duy trì và lịch sử CVE, và được ghim qua baseline của vcpkg. Pak là dữ liệu
từ ngoài (tải từ CDN, nằm trên đĩa người chơi), nên bộ giải nén đứng ngay ranh giới tin cậy.

## Quyết định

| | zstd |
|---|---|
| Port vcpkg, bản ghim | `zstd` 1.5.7 |
| Baseline vcpkg | `07f4812200df3d3c931c0c8a6081d3b21fe2bf9f` |
| Repo | https://github.com/facebook/zstd |
| License | BSD-3-Clause OR GPL-2.0-only; dự án dùng nhánh BSD |
| Người duy trì | Meta Platforms (org `facebook`) |
| Bản mới nhất | v1.5.7, commit của tag ngày 2025-02-18 (bằng bản ghim) |
| Lịch sử CVE | 4 CVE, bản sửa muộn nhất có từ tag `v1.5.4`; bản ghim không bị ảnh hưởng |

1. `zstd` vào `vcpkg.json`; CMake tìm bằng `find_package(zstd CONFIG)`, target `zstd::libzstd`,
   link PRIVATE vào `engine_io`.
2. Chỉ `engine/io` include `<zstd.h>` và `<zdict.h>`. Header public của `engine/io` không lộ kiểu
   nào của zstd (X.2).
3. Port bật `ZSTD_LEGACY_SUPPORT=1`, nên bộ giải nén còn nhận frame của các định dạng v0.x cũ.
   `engine/io` chỉ đưa cho zstd những entry đã khớp hash BLAKE2b trong index đã kiểm của pak, và chỉ
   nhận frame bắt đầu bằng magic của định dạng hiện hành (`0xFD2FB528`); dữ liệu tới được bộ giải
   nén vì vậy luôn do cooker của dự án sinh ra. Định dạng pak được ghi ở `docs/formats/pak.md`,
   thêm cùng code đọc pak của `engine/io`.
4. Port build bằng CMake (`vcpkg_cmake_configure`), không cần công cụ hệ thống nào thêm.

## Bằng chứng

- License: ở tag `v1.5.7`, `LICENSE` mở đầu "BSD License / For Zstandard software", `COPYING` là
  "GNU GENERAL PUBLIC LICENSE Version 2", `README.md` ghi "Zstandard is dual-licensed under BSD OR
  GPLv2"; `ports/zstd/vcpkg.json` ở baseline ghi `BSD-3-Clause OR GPL-2.0-only` (bản clone git
  ngày 2026-09-27).
- Bảo trì: tag `v1.5.7` là commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99` (2025-02-18) và là tag
  phát hành mới nhất; nhánh `dev` có 42 commit trong 90 ngày tới 2026-09-27, commit cuối
  2026-09-18.
- CVE, tra ngày 2026-09-27 trên bản sao NVD `fkie-cad/nvd-json-data-feeds` (bản
  `v2026.09.27-000023`, 398 400 bản ghi; nvd.nist.gov và cve.org bị proxy của phiên dựng chặn):
  regex `zstd|zstandard|facebook/zstd` ra 49 bản ghi, chỉ 4 bản ghi có CPE `facebook:zstandard`:
  - CVE-2019-11922: race trong hàm nén một lượt, ghi quá vùng nhớ khi buffer ra nhỏ hơn cỡ khuyên
    dùng; trước 1.3.8. NVD dẫn commit `3e5cdf1`, có từ tag `v1.3.8`.
  - CVE-2021-24031, CVE-2021-24032: quyền của tệp do công cụ dòng lệnh `zstd` tạo; chỉ CLI, trước
    1.4.1 và 1.4.9.
  - CVE-2022-4899: CLI tràn buffer khi nhận chuỗi rỗng (issue #3200); sửa ở merge `28ceb63` (PR
    #3220), có từ tag `v1.5.4`.
  Cả `3e5cdf1` và `28ceb63` là tổ tiên của `v1.5.7` (`git merge-base --is-ancestor`). Các bản ghi
  còn lại là sản phẩm khác nhúng hoặc bọc zstd.
- Công thức port: `ports/zstd/portfile.cmake` ở baseline trên (`ZSTD_LEGACY_SUPPORT=1`,
  `ZSTD_MULTITHREAD_SUPPORT=1`, không build CLI trừ khi bật feature `tools`).

## Hệ quả

- Container dựng không tải được nguồn port vcpkg; build cục bộ dùng gói Ubuntu `libzstd-dev`, chỉ CI
  là bằng chứng cho bản ghim (NGHI-NGO-026).
- Không có bản phát hành mới trong khoảng 19 tháng: mỗi lần nâng baseline, tra lại CVE và các sửa
  lỗi bảo mật trên nhánh `dev`.
- Nâng phiên bản là đổi baseline vcpkg, kèm tra lại CVE, trong cùng commit.

## Phương án đã loại

- Bộ nén khác (LZ4, Brotli...): ARCH §4.1 đã chọn Zstd, kể cả dictionary cho file nhỏ; đổi cần ADR
  thay ADR này.
- Tự viết bộ nén: không có lợi so với chi phí và rủi ro.
