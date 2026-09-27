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

- Tài liệu nền, ADR 0001–0012 (0009–0012 chờ xác nhận) và sổ nghi ngờ.
- Cổng kiểm `check_style`, `check_layers`, `check_tracked`, kèm unittest.
- Khung CMake: preset của ARCH §6, toolchain và triplet vcpkg riêng, `orion_add_module()`,
  `tests/toolchain/features.cpp` (số đo từng toolchain ở NGHI-NGO-009).
- CI GitHub Actions theo ARCH §8 (ADR 0009).
- `engine/core`: kiểu số, macro nền tảng, `ORION_ASSERT`/`ORION_VERIFY`, `orion::narrow`, `Error`
  và `Result`, `Arena`, `Handle`/`SlotMap`, đọc ghi byte little-endian, UTF-8, thời gian và đồng
  hồ tiêm được, hàng đợi không khoá, log bất đồng bộ (text và JSON lines); bộ đếm cấp phát cho test
  (X.7).
- `engine/math`: vector, quaternion, `WorldPos` f64 (ADR 0001), lượng giác tất định làm tròn trung
  thành (số đo trong `trig.hpp`, giữ bằng golden test và bảng làm tròn đúng của
  `tools/gen_math_reference.py`), PCG32 và SplitMix64.
- `engine/jobs`: luồng có tên (API gốc của từng nền tảng, ADR 0007), job system work-stealing với
  deque Chase–Lev, `parallel_for` chia khối cố định để gộp kết quả tất định, không khoá và không cấp
  phát sau khi dựng. Cầu nối JobSystem cho Jolt đến cùng `engine/physics`.
- `engine/crypto` bọc libsodium (ADR 0011): BLAKE2b, chữ ký Ed25519, trao khoá X25519 ra hai khoá
  phiên, AEAD ChaCha20-Poly1305 với nonce lấy từ số thứ tự, băm mật khẩu Argon2id với chuỗi PHC
  được kiểm trước khi tới libsodium; so sánh thời gian hằng và bí mật tự xoá. Test theo vector của
  RFC 8032, 7748, 8439 và chuỗi Argon2id của bản cài đặt tham chiếu.
- `engine/io`: đường dẫn ảo chung cho tệp rời và pak, tệp đọc theo vị trí và ghi nguyên tử (tệp tạm,
  đẩy xuống đĩa, đổi tên đè); build và test trên Windows và Linux, build cho Android; bản Apple có
  code nhưng chưa build lần nào (NGHI-NGO-032). Pak phiên bản 1 (`docs/formats/pak.md`, ADR 0012):
  hash của header và index so với manifest trước khi phân tích, mỗi entry nén Zstd riêng (có
  dictionary) và được băm trước khi giải nén; đọc từ nhiều luồng, không cấp phát sau khi dành bộ
  đệm. Bộ ghi pak và huấn luyện dictionary chỉ có ở DEV và tool. Manifest đã ký Ed25519
  (`docs/formats/manifest.md`): người ký phải nằm trong danh sách khoá tin cậy và chữ ký được kiểm
  trước khi đọc bản ghi pak; bộ ghi manifest chỉ có ở DEV và tool. VFS: mount pak của manifest đã ký
  (kiểm cỡ và `index_hash` khi mount), mount sau che mount trước, đọc từ nhiều luồng; thư mục tệp
  rời và pak ngoài manifest chỉ có ở DEV, và `engine/io/tests/ship_api_check.cpp` giữ luật đó ở mọi
  preset bằng cách biên dịch với cấu hình ship. Đọc bất đồng bộ: luồng `orion-io` đọc qua VFS theo
  thứ tự gửi; luồng chính gửi và nhận không cấp phát, không chặn, và số yêu cầu chưa nhận có giới
  hạn.
- Khung fuzz `tests/fuzz/` (target: chuỗi băm mật khẩu, index pak, manifest), `tools/run_fuzz.py`
  và job fuzz đêm; ở mọi preset khác ctest chạy lại corpus.
