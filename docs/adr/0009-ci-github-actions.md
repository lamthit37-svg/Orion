# ADR 0009 — CI trên GitHub Actions, thư mục `.github/`

- **Trạng thái:** Đề xuất — cần người quyết xác nhận (CLAUDE.md X.16.2: thêm thư mục cấp cao)
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §2, §8; CLAUDE.md X.1, X.2, X.12

## Bối cảnh

ARCH §8 đặt ra CI tối thiểu, nhưng cây thư mục ở ARCH §2 không có chỗ cho cấu hình CI. Repo nằm
trên GitHub, và GitHub Actions chỉ đọc workflow trong `.github/workflows/`. Ngoài ra, bốn trong năm
toolchain của CLAUDE.md X.12 (MSVC, clang-cl, NDK, Apple clang) không có trên máy dựng Linux; runner
Windows, Linux và macOS của GitHub là nơi duy nhất đo được chúng mà không cần máy riêng.

## Quyết định

1. CI chạy trên GitHub Actions. Thêm thư mục cấp cao `.github/` gồm:
   - `workflows/ci.yml` — các job của ARCH §8, mỗi job ghi số mục §8 mà nó thực hiện;
   - `actions/setup-orion/` — bước chuẩn bị dùng chung: Python 3.13, CMake và Ninja đúng bản,
     clang-format và clang-tidy ghim bản, vcpkg ở đúng baseline của `vcpkg.json`, cache nhị phân của
     vcpkg theo triplet.
2. Action bên ngoài chỉ lấy từ tổ chức `actions/` của GitHub và được ghim theo SHA của commit, kèm
   tên tag trong comment.
3. Baseline vcpkg của CI đọc từ `vcpkg.json`, không khai lần thứ hai.
4. Job iOS chạy trên runner macOS chỉ khi được gọi tay (`workflow_dispatch` với `ios: true`), cho
   tới khi người quyết bật hẳn (ARCH §8 mục 8, NGHI-NGO-008).
5. Mục của ARCH §8 chưa có gì để chạy (benchmark, fuzz đêm, ảnh vàng, ship đêm, migration) được
   thêm vào workflow cùng commit với code mà nó kiểm.

## Hệ quả

- Mỗi push chạy cổng kiểm trước, rồi các job build song song trên Linux, Windows và Android.
- Số đo trên toolchain 1, 2, 4 (và 5 khi gọi tay) lấy từ log CI để đóng các mục sổ nghi ngờ.
- Chuyển CI sang nền tảng khác nghĩa là viết ADR mới thay ADR này.

## Phương án đã loại

- Không có CI cho tới khi có máy riêng: không đo được bốn toolchain, trái ARCH §8.
- Đặt script CI trong `tools/`: GitHub không đọc workflow ngoài `.github/workflows/`.
