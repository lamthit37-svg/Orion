# ADR 0011 — Dependency: libsodium

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.9, X.16.2: thêm dependency cần
  ADR)
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.1, §4.5; CLAUDE.md X.2, X.9; NGHI-NGO-026

## Bối cảnh

ARCH §4.1 chọn libsodium cho mọi nhu cầu crypto và hash: X25519, AEAD, Ed25519, BLAKE2b, Argon2id,
"một thư viện cho tất cả", bọc trong `engine/crypto`. CLAUDE.md X.9 đòi mỗi dependency mới có ADR
ghi license, người duy trì và lịch sử CVE, và được ghim qua baseline của vcpkg. Đây là thư viện
bảo mật trên đường nóng của sản phẩm (kênh realtime, token, pak, mật khẩu), nên lịch sử CVE được tra
kỹ hơn thường lệ.

## Quyết định

| | libsodium |
|---|---|
| Port vcpkg, bản ghim | `libsodium` 1.0.22, port-version 1 |
| Baseline vcpkg | `07f4812200df3d3c931c0c8a6081d3b21fe2bf9f` |
| Repo | https://github.com/jedisct1/libsodium |
| License | ISC |
| Người duy trì | Frank Denis (`jedisct1`), một cá nhân; không có SECURITY.md |
| Bản mới nhất | 1.0.22, tag ngày 2026-04-10 (bằng bản ghim) |
| Lịch sử CVE | CVE-2025-69277, đã sửa ở 1.0.21; bản ghim không bị ảnh hưởng |

1. `libsodium` vào `vcpkg.json`; CMake tìm bằng `find_package(unofficial-sodium CONFIG)` (tên do
   port vcpkg đặt), target `unofficial-sodium::sodium`, link PRIVATE vào `engine_crypto`.
2. Chỉ `engine/crypto` include header của libsodium (CLAUDE.md X.2; `tools/check_layers.py` gán cả
   `sodium.h` lẫn `sodium/` cho module này). Mỗi tệp include đúng header con cung cấp tên nó dùng
   (`<sodium/crypto_kx.h>`...), như `misc-include-cleaner` đòi; các header con tự include đủ thứ
   chúng cần (kiểm trên tag `1.0.22`). Header public của `engine/crypto` không lộ kiểu nào của
   libsodium.
3. Thuật toán dùng: BLAKE2b (`crypto_generichash`), Ed25519 (`crypto_sign`), trao khoá X25519
   (`crypto_kx`, và `crypto_scalarmult_base` để tính khoá công khai từ khoá bí mật đã lưu),
   ChaCha20-Poly1305 bản IETF (`crypto_aead_chacha20poly1305_ietf`), Argon2id (`crypto_pwhash`),
   sinh số ngẫu nhiên (`randombytes_buf`), so sánh thời gian hằng (`sodium_memcmp`), xoá bộ nhớ
   (`sodium_memzero`). Không dùng `crypto_core_ed25519_is_valid_point` (hàm của CVE-2025-69277).
4. Trên Linux, Android, iOS, port build bằng autotools (`vcpkg_make_configure(AUTORECONF)`), nên máy
   dựng cần autoconf, automake, libtool. Trên Windows với MSVC, port build bằng msbuild.

## Bằng chứng

- License: tệp `LICENSE` ở tag `1.0.22` mở đầu "ISC License, Copyright (c) 2013-2026 Frank Denis".
- Bảo trì: 1.0.22 là bản mới nhất (tag 2026-04-10; 1.0.21 tag 2026-01-06); 24 commit trong 90 ngày
  tới 2026-09-27, commit cuối 2026-09-22 (bản clone git ngày 2026-09-27).
- CVE, tra ngày 2026-09-27 trên bản sao NVD `fkie-cad/nvd-json-data-feeds` (bản
  `v2026.09.27-000023`; nvd.nist.gov và cve.org bị proxy của phiên dựng chặn), cộng
  `github.com/advisories`: regex `libsodium|jedisct1|\bsodium\b` ra 16 bản ghi, chỉ CVE-2025-69277
  là của chính libsodium; các bản ghi khác là sản phẩm chỉ nhắc tới libsodium (binding Perl,
  GameNetworkingSockets, Nim, WAL-G, Unbound, Vim).
- CVE-2025-69277 (công bố 2025-12-31): `crypto_core_ed25519_is_valid_point()` có thể nhận điểm ngoài
  nhóm con chính; chỉ ảnh hưởng cách dùng bất thường (crypto tự chế, hoặc đưa điểm không tin cậy vào
  hàm này). Sửa ở commit `ad3004e`, có trong 1.0.21; commit sửa của nhánh stable `f2da4cd` là tổ
  tiên của tag `1.0.22` (kiểm bằng git).
- Công thức port: `ports/libsodium/portfile.cmake` và `sodiumConfig.cmake.in` ở baseline trên.

## Hệ quả

- Build cục bộ trong container không tải được nguồn port vcpkg dùng gói Ubuntu `libsodium-dev`
  1.0.18 qua một cấu hình CMake riêng máy; chỉ CI là bằng chứng cho bản ghim (NGHI-NGO-026). Mọi
  hàm ở mục 3 đều có từ 1.0.18.
- Nâng phiên bản là đổi baseline vcpkg, kèm tra lại CVE, trong cùng commit.
- Người duy trì là một cá nhân: theo dõi GitHub advisories của repo mỗi lần nâng baseline.

## Phương án đã loại

- OpenSSL, BoringSSL, Monocypher: ARCH §4.1 đã chọn libsodium vì một thư viện phủ đủ mọi thuật toán
  cần dùng với API khó dùng sai; đổi thư viện cần một ADR thay ADR này.
- Tự viết thuật toán: CLAUDE.md X.9 cấm tự chế thuật toán mã hoá.
