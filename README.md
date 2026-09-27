# Orion

Engine và game MMO viết bằng C++23: client Windows, Android, iOS; server Linux; dịch vụ HTTP cùng
một ngôn ngữ (ADR 0008).

| Đọc gì | Ở đâu |
|---|---|
| Luật viết code | [CLAUDE.md](CLAUDE.md) |
| Bố cục, tầng, công nghệ, ba vùng DEV, CHẠY, CHƠI | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Quyết định trụ cột | [docs/adr/](docs/adr/README.md) |
| Những gì chưa đo | [docs/NGHI-NGO.md](docs/NGHI-NGO.md) |

## Dựng

Cần có:

- CMake 4.4 trở lên, Ninja, Python 3.13;
- vcpkg, với biến môi trường `VCPKG_ROOT` trỏ tới bản clone; dự án ghim baseline trong
  `vcpkg.json`;
- clang-format 20.1.8 và clang-tidy 20.1.0, cài bằng
  `pip install clang-format==20.1.8 clang-tidy==20.1.0`;
- Linux (WSL2 hoặc CI): clang 20 trở lên và libstdc++ 14 trở lên (NGHI-NGO-015);
- Windows: MSVC, chạy mọi lệnh bên trong cổng `dev.bat` (hiến pháp II).

| Việc | Lệnh |
|---|---|
| Dựng và test trên Linux | `cmake --workflow --preset linux` |
| Dựng và test trên Windows | `cmake --workflow --preset dev` |
| Race detector trên Linux | `cmake --workflow --preset linux-tsan` |
| Coverage (X.4) | build và `ctest --preset linux-coverage`, rồi `tools/check_coverage.py out/build/linux-coverage` |
| Cổng kiểm | `python3.13 tools/check_style.py`, `check_layers.py`, `check_tracked.py` |
| Sửa format tự động | `python3.13 tools/check_style.py --fix` |

Trên Windows gọi script bằng `py` thay cho `python3.13` (hiến pháp VIII). Cổng kiểm cũng chạy tự
động trong mọi lần build qua target `orion_check_style`; cổng đỏ thì build dừng.

Mọi thứ sinh ra nằm trong `out/` và xoá lúc nào cũng được (ARCH §1).

## Trạng thái

Repo đang được dựng theo ARCH. Mục này liệt kê đúng những gì đã build và test được; vùng CHẠY và
CHƠI chưa có.

- Tài liệu nền, ADR 0001–0008 và sổ nghi ngờ.
- Cổng kiểm `check_style`, `check_layers`, `check_tracked`, kèm unittest.
- Khung CMake: preset của ARCH §6, toolchain và triplet vcpkg riêng, `orion_add_module()`,
  `tests/toolchain/features.cpp` (số đo từng toolchain ở NGHI-NGO-009).
- CI GitHub Actions theo ARCH §8 (ADR 0009).
- `engine/core`: kiểu số, macro nền tảng, `ORION_ASSERT`/`ORION_VERIFY`, `orion::narrow`, `Error`
  và `Result`, `Arena`, `Handle`/`SlotMap`, đọc ghi byte little-endian, UTF-8, thời gian và đồng
  hồ tiêm được, hàng đợi không khoá, log bất đồng bộ (text và JSON lines); bộ đếm cấp phát cho test
  (X.7).
