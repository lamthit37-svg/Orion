# Sổ nghi ngờ

Luật của sổ nằm ở CLAUDE.md X.6. Mỗi thứ chưa đo là một mục ở đây, không phải một câu trong
comment. Code dựa trên giả định chưa kiểm thì ghi mã mục tại chỗ: `// NGHI-NGO-012: <giả định>`.

Mẫu một mục:

- **Mở:** ngày mở.
- **Khẳng định:** điều cần kiểm, viết sao cho đo được đúng hoặc sai.
- **Lý do nghi:** vì sao chưa tin.
- **Cách kiểm:** lệnh cụ thể.
- **Trạng thái:** mở, hoặc đóng kèm ngày.
- **Bằng chứng:** chỉ khi đóng — lệnh đã chạy, kết quả, hoặc link nguồn.

Máy đo mặc định của các mục đã đóng dưới đây, trừ khi mục ghi khác: container Linux của phiên
dựng, Ubuntu 24.04.4 LTS, kernel 6.18.44 x86_64, 4 core, 15 GiB RAM; clang 20.1.2 của Ubuntu
(`clang++-20`) với libstdc++ 14; CMake 4.4.3; Ninja 1.11.1; Python 3.13.12; vcpkg ở commit
`07f4812200df3d3c931c0c8a6081d3b21fe2bf9f`.

---

## Đang mở

### NGHI-NGO-001 — Port vcpkg build được với triplet riêng của dự án

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi port ở ARCH §4 build được với triplet trong `cmake/triplets/` trên từng nền
  tảng đích: `x64-linux-orion`, `x64-windows-orion`, `arm64-android-orion`, `arm64-ios-orion`.
- **Lý do nghi:** Tên và phiên bản port đã đo (NGHI-NGO-016), nhưng việc build với cờ riêng
  (`-ffp-contract=off`, CRT tĩnh, toolchain chainload) thì chưa. Mỗi port chỉ được coi là đo xong
  khi nó build xanh trong CI của nền tảng tương ứng.
- **Cách kiểm:** `cmake --preset linux` (vcpkg cài theo manifest), tương tự cho `dev`,
  `android-arm64`, `ios-arm64`; đọc log `vcpkg install` trong CI.
- **Trạng thái:** mở. Đóng từng phần: mỗi port build xanh thì ghi dòng bằng chứng vào đây.

### NGHI-NGO-002 — Thư viện ngoài build được khi tắt exception và RTTI

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi thư viện ở ARCH §4 dùng được từ code build với `-fno-exceptions -fno-rtti`
  (MSVC: `/EHs-c- /GR-`). Riêng Boost.Beast và Boost.Asio: hoặc dùng được với
  `BOOST_NO_EXCEPTIONS`, hoặc `server/lib/http` phải là module bật exception và bắt hết ở ranh
  giới (CLAUDE.md X.3).
- **Lý do nghi:** ARCH §9. Boost.Asio ném exception ở nhiều API không có overload `error_code`.
- **Cách kiểm:** build `server/lib/http` với `BOOST_NO_EXCEPTIONS` và một hàm
  `boost::throw_exception` gọi `ORION_VERIFY`; chạy test HTTP dưới ASan.
- **Trạng thái:** mở.

### NGHI-NGO-003 — simdjson: API mã lỗi đủ dùng khi tắt exception

- **Mở:** 2026-09-27
- **Khẳng định:** Parser request HTTP viết được hoàn toàn bằng API `simdjson_result`/`error_code`
  với `SIMDJSON_EXCEPTIONS=0`.
- **Lý do nghi:** ARCH §9. Một số tiện ích của simdjson chỉ có bản ném exception.
- **Cách kiểm:** build parser request với `-DSIMDJSON_EXCEPTIONS=0 -fno-exceptions`; fuzz 10 phút.
- **Trạng thái:** mở.

### NGHI-NGO-004 — clang và TSan trong WSL2

- **Mở:** 2026-09-27
- **Khẳng định:** Preset `linux-tsan` chạy được trong WSL2 trên máy dev Windows.
- **Lý do nghi:** Hiến pháp II.2 chỉ nói TSan không có trên clang-cl. Đường Linux đã đo trên
  container (NGHI-NGO-018), nhưng WSL2 có kernel và cấu hình ASLR riêng, mà runtime của TSan phụ
  thuộc bố cục bộ nhớ ảo do kernel quyết định.
- **Cách kiểm:** trong WSL2: `cmake --workflow --preset linux-tsan`; nếu TSan báo
  `unexpected memory mapping` thì ghi giá trị `sysctl vm.mmap_rnd_bits`.
- **Trạng thái:** mở.

### NGHI-NGO-005 — CMake phát `/external:I` cho include SYSTEM với MSVC

- **Mở:** 2026-09-27
- **Khẳng định:** Với MSVC, target IMPORTED của vcpkg được include bằng `/external:I`, nên
  `/external:W0` chặn được warning từ header ngoài mà không phải hạ `/W4 /WX`.
- **Lý do nghi:** ARCH §9. Chưa có máy Windows trong phiên dựng.
- **Cách kiểm:** job Windows của CI in `compile_commands.json` của preset `dev`; tìm `/external:I`
  trên dòng biên dịch một file include libsodium.
- **Trạng thái:** mở.

### NGHI-NGO-006 — Cờ MSVC tắt hợp nhất FMA

- **Mở:** 2026-09-27
- **Khẳng định:** Với `/fp:precise` và không có `/fp:contract`, MSVC không sinh lệnh FMA từ biểu
  thức `a * b + c`, kể cả khi bật `/arch:AVX2`.
- **Lý do nghi:** ARCH §9, CLAUDE.md X.11. Chưa đo trên toolchain của dự án, và chưa đối chiếu với
  tài liệu chính thức của MSVC về `/fp:contract`.
- **Cách kiểm:** job Windows của CI biên dịch `double f(double a, double b, double c) { return a *
  b + c; }` với `/O2 /fp:precise /arch:AVX2 /FA`, rồi tìm `vfmadd` trong file `.asm`.
- **Trạng thái:** mở.

### NGHI-NGO-007 — Android SDK, NDK và JDK 25

- **Mở:** 2026-09-27
- **Khẳng định:** Android Gradle Plugin bản dùng cho dự án chạy được với JDK 25; NDK có clang và
  libc++ đủ phần giao C++23 ở `tests/toolchain/features.cpp`.
- **Lý do nghi:** ARCH §9. Máy dev chưa có Android SDK và NDK.
- **Cách kiểm:** cài SDK và NDK; `cmake --preset android-arm64` rồi build `tests/toolchain`;
  `gradlew bundleRelease` trong `game/client/android/` với `JAVA_HOME` trỏ JDK 25.
- **Trạng thái:** mở.

### NGHI-NGO-008 — Chưa có Mac cho iOS

- **Mở:** 2026-09-27
- **Khẳng định:** Preset `ios-arm64` sinh được dự án Xcode và build được `tests/toolchain` bằng
  Apple clang.
- **Lý do nghi:** ARCH §9. Chưa có Mac; runner macOS của CI chưa được bật cho dự án.
- **Cách kiểm:** trên Mac: `cmake --preset ios-arm64 && cmake --build --preset ios-arm64 --target
  toolchain_features`.
- **Trạng thái:** mở.

### NGHI-NGO-009 — Phần giao C++23 của năm toolchain

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi `static_assert` trong `tests/toolchain/features.cpp` xanh trên cả năm
  toolchain ở CLAUDE.md X.12.
- **Lý do nghi:** Hiến pháp III.1 chỉ đo toolchain 1. Toolchain 3 đã đo trong phiên dựng
  (NGHI-NGO-015, NGHI-NGO-019); bốn cái còn lại chưa.
- **Cách kiểm:** CI build target `toolchain_features` trên MSVC, clang-cl, clang Linux, NDK và
  Apple clang.
- **Trạng thái:** mở. Toolchain 3: xanh (NGHI-NGO-019).

### NGHI-NGO-010 — Điều khoản license hiện hành của FMOD

- **Mở:** 2026-09-27
- **Khẳng định:** License FMOD cho phép phát hành game online có thu phí trên Windows, Android và
  iOS với quy mô của dự án; ghi rõ bậc phí.
- **Lý do nghi:** ARCH §9. FMOD là phần mềm độc quyền, điều khoản đổi theo năm.
- **Cách kiểm:** đọc trang license chính thức của FMOD tại ngày kiểm, lưu bản PDF vào hồ sơ pháp
  lý (không vào repo), ghi link và ngày vào đây.
- **Trạng thái:** mở.

### NGHI-NGO-011 — Ngân sách ở CLAUDE.md X.8

- **Mở:** 2026-09-27
- **Khẳng định:** Các con số ở X.8 đạt được trên máy tham chiếu của ADR 0005.
- **Lý do nghi:** Đều là giả định cho tới lần đo đầu tiên; máy tham chiếu chưa chốt.
- **Cách kiểm:** benchmark và metric telemetry tương ứng từng dòng của X.8, chạy trên máy tham
  chiếu với preset `profile`.
- **Trạng thái:** mở.

### NGHI-NGO-012 — Hiến pháp máy không có trong repo

- **Mở:** 2026-09-27
- **Khẳng định:** Những gì repo này suy ra từ các chỗ CLAUDE.md và ARCH trỏ tới hiến pháp khớp với
  hiến pháp thật: nội dung `tools/check_style.py` (V.5), `.clang-format` (V.4), quy ước đặt tên
  (IV, IV.1), danh sách tính năng đo cho MSVC (III.1), cổng `dev.bat` (II), lệnh `py` (VIII).
- **Lý do nghi:** Phiên dựng không có bản hiến pháp. `tools/check_style.py` được ghi là "cổng hiện
  có" nhưng repo trống, nên bản trong repo được viết mới theo những gì CLAUDE.md mô tả.
- **Cách kiểm:** đối chiếu từng mục trên với hiến pháp; chỗ nào lệch thì sửa repo theo hiến pháp.
- **Trạng thái:** mở.

### NGHI-NGO-013 — clang-format trên máy dev Windows khớp phiên bản ghim

- **Mở:** 2026-09-27
- **Khẳng định:** Máy dev Windows dùng đúng clang-format 20.1.8 như CI, nên cổng kiểm cho cùng kết
  quả ở hai nơi.
- **Lý do nghi:** Kết quả format đổi giữa các bản clang-format. Visual Studio đi kèm một bản LLVM
  riêng, chưa biết là bản nào.
- **Cách kiểm:** trong `dev.bat`: `clang-format --version`; nếu lệch thì
  `py -m pip install clang-format==20.1.8` và đặt biến `ORION_CLANG_FORMAT` trỏ tới bản đó.
- **Trạng thái:** mở.

### NGHI-NGO-014 — WSL2 có clang ≥ 20 và libstdc++ ≥ 14

- **Mở:** 2026-09-27
- **Khẳng định:** Bản phân phối Linux trong WSL2 của máy dev cài được clang 20 trở lên và
  libstdc++ 14 trở lên, như container dựng.
- **Lý do nghi:** clang 18 với libstdc++ 13 không có `std::expected` (NGHI-NGO-015); chưa biết bản
  phân phối trong WSL2.
- **Cách kiểm:** trong WSL2: `clang++-20 --version`, rồi `cmake --workflow --preset linux`.
- **Trạng thái:** mở.

### NGHI-NGO-020 — Thiết bị Android ở sàn có descriptor indexing

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi thiết bị Android trên sàn của ADR 0005 hỗ trợ Vulkan 1.2 với
  `runtimeDescriptorArray`, `descriptorBindingPartiallyBound` và chỉ số không đồng nhất cho mảng
  sampled image, đủ cho bindless ở ADR 0003.
- **Lý do nghi:** Sàn Android chưa chốt; driver Vulkan trên mobile lệch nhau nhiều.
- **Cách kiểm:** gom `vulkaninfo --json` từ các thiết bị của thị trường mục tiêu (hoặc dữ liệu
  GPUInfo), lọc các trường trên.
- **Trạng thái:** mở.

### NGHI-NGO-021 — GPU ở sàn PC có Shader Model 6.6

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi GPU trên sàn PC của ADR 0005 chạy được Shader Model 6.6 với Agility SDK đi
  kèm game, đủ cho `ResourceDescriptorHeap[]` ở ADR 0003.
- **Lý do nghi:** Sàn PC chưa chốt; Shader Model 6.6 phụ thuộc cả phần cứng lẫn driver.
- **Cách kiểm:** khảo sát phần cứng thật; trên máy mẫu, gọi `CheckFeatureSupport` với
  `D3D12_FEATURE_SHADER_MODEL` và `D3D_SHADER_MODEL_6_6`.
- **Trạng thái:** mở.

### NGHI-NGO-022 — Thị phần iOS 26 trong thị trường mục tiêu

- **Mở:** 2026-09-27
- **Khẳng định:** Tỉ lệ người chơi mục tiêu dùng iOS 26 trở lên đủ cao để bỏ Metal 3 (ADR 0005).
- **Lý do nghi:** Chưa có số liệu thị phần phiên bản iOS của thị trường mục tiêu.
- **Cách kiểm:** số liệu phân bố phiên bản iOS của thị trường mục tiêu tại thời điểm chốt.
- **Trạng thái:** mở.

---

## Đã đóng

### NGHI-NGO-015 — clang 18 với libstdc++ 13 không có `std::expected`

- **Mở:** 2026-09-27
- **Khẳng định:** clang 18 đi kèm Ubuntu 24.04 không dùng được `std::expected` của libstdc++ 13,
  nên toolchain 3 phải là clang ≥ 20 với libstdc++ ≥ 14.
- **Lý do nghi:** `<expected>` của libstdc++ đòi `__cpp_concepts >= 202002L`.
- **Cách kiểm:** biên dịch một file dùng `std::expected<int, int>` với `-std=c++23`.
- **Trạng thái:** đóng 2026-09-27.
- **Bằng chứng:** `clang++ -std=c++23 e.cpp` (Ubuntu clang 18.1.3, libstdc++ 13) báo
  `error: no member named 'expected' in namespace 'std'`. Cùng file với `clang++-20 -std=c++23`
  (clang 20.1.2, tự chọn GCC installation 14) biên dịch và chạy đúng; `__cpp_lib_expected` =
  202211, `__cpp_concepts` = 202002. `cmake/toolchains/linux-clang.cmake` vì vậy từ chối clang
  dưới 20.

### NGHI-NGO-016 — Tên và phiên bản port vcpkg của các thư viện ở ARCH §4

- **Mở:** 2026-09-27
- **Khẳng định:** Mỗi thư viện ở ARCH §4 có port vcpkg ở baseline `07f4812`, với tên và phiên bản
  ghi dưới đây.
- **Lý do nghi:** ARCH §9: tên port chưa kiểm.
- **Cách kiểm:** đọc `ports/<tên>/vcpkg.json` trong clone vcpkg ở commit baseline.
- **Trạng thái:** đóng 2026-09-27.
- **Bằng chứng:** có port: `libsodium` 1.0.22#1, `zstd` 1.5.7, `gtest` 1.18.0, `benchmark` 1.9.5,
  `simdjson` 4.6.11, `boost-beast` 1.92.0, `boost-asio` 1.92.0, `libpq` 18.4, `hiredis` 1.4.1,
  `entt` 3.16.0, `joltphysics` 5.6.0#1, `luau` 0.739, `sdl3` 3.4.16#1, `tracy` 0.14.1,
  `sentry-native` 0.17.1, `opentelemetry-cpp` 1.29.0#1, `nlohmann-json` 3.12.0#2,
  `json-schema-validator` 2.4.0, `meshoptimizer` 1.3, `fastgltf` 0.9.0, `ktx` 4.4.2,
  `recastnavigation` 1.6.0#1 (gồm Recast, Detour, DetourTileCache), `imgui` 1.92.9, `rmlui` 6.3,
  `freetype` 2.14.3, `harfbuzz` 14.5.0, `volk` 1.4.357.0, `vulkan-memory-allocator` 3.4.0,
  `d3d12-memory-allocator` 3.2.0, `directx-headers` 1.619.5, `directx12-agility` 1.619.5,
  `directx-dxc` 2026-05-27. Không có port: `ufbx`, `ozz-animation`, `astc-encoder`, `bc7enc-rdo`,
  `metal-cpp`; theo ARCH §9 các thư viện này vào `third_party/`, ghim commit, kèm ADR, khi module
  đầu tiên cần tới chúng. FMOD là phần mềm độc quyền, không qua vcpkg.

### NGHI-NGO-017 — clang hợp nhất `a * b + c` thành FMA nếu không có `-ffp-contract=off`

- **Mở:** 2026-09-27
- **Khẳng định:** clang 20 mặc định hợp nhất phép nhân và cộng thành FMA khi CPU đích có FMA, nên
  cờ `-ffp-contract=off` ở CLAUDE.md X.11 là bắt buộc chứ không thừa.
- **Lý do nghi:** Mặc định `-ffp-contract` của clang đổi qua các phiên bản.
- **Cách kiểm:** `clang++-20 -O2 -march=haswell -S` hàm `a * b + c`, có và không có
  `-ffp-contract=off`.
- **Trạng thái:** đóng 2026-09-27.
- **Bằng chứng:** không có cờ: sinh `vfmadd213sd`. Có `-ffp-contract=off`: sinh `vmulsd` rồi
  `vaddsd`.

### NGHI-NGO-018 — TSan, ASan, UBSan và libFuzzer chạy được với clang 20 trên Linux

- **Mở:** 2026-09-27
- **Khẳng định:** Preset `linux-tsan` và các target fuzz (libFuzzer kèm ASan, UBSan) chạy được với
  clang 20 trên Linux x86-64.
- **Lý do nghi:** ARCH §9; runtime sanitizer phụ thuộc kernel và gói `libclang-rt-20-dev`.
- **Cách kiểm:** một chương trình có data race dựng với `-fsanitize=thread`; một fuzz target có lỗi
  cố ý dựng với `-fsanitize=fuzzer,address,undefined`.
- **Trạng thái:** đóng 2026-09-27. Chỉ đóng cho Linux gốc; WSL2 còn mở ở NGHI-NGO-004.
- **Bằng chứng:** TSan in `WARNING: ThreadSanitizer: data race` đúng dòng có race;
  `vm.mmap_rnd_bits` = 28. libFuzzer tìm ra input gây lỗi trong vài giây và ASan in
  `SUMMARY: AddressSanitizer: SEGV` đúng dòng.

### NGHI-NGO-019 — Tính năng C++23 của toolchain 3 (clang 20, libstdc++ 14)

- **Mở:** 2026-09-27
- **Khẳng định:** Toolchain 3 có các feature-test macro ghi dưới đây, với giá trị ghi dưới đây.
- **Lý do nghi:** CLAUDE.md X.12: phải đo, không dựa trí nhớ.
- **Cách kiểm:** in giá trị từng macro sau `#include <version>` với `clang++-20 -std=c++23`; sau
  đó `tests/toolchain/features.cpp` giữ lại đúng phần dự án dùng dưới dạng `static_assert`.
- **Trạng thái:** đóng 2026-09-27 cho toolchain 3. Bốn toolchain còn lại ở NGHI-NGO-009.
- **Bằng chứng:** có: `__cpp_lib_expected` 202211, `__cpp_lib_print` 202211,
  `__cpp_lib_generator` 202207, `__cpp_lib_stacktrace` 202011, `__cpp_lib_format` 202110,
  `__cpp_lib_bit_cast` 201806, `__cpp_lib_byteswap` 202110, `__cpp_lib_to_underlying` 202102,
  `__cpp_lib_unreachable` 202202, `__cpp_lib_span` 202002, `__cpp_lib_ranges` 202211,
  `__cpp_lib_source_location` 201907, `__cpp_lib_jthread` 201911,
  `__cpp_lib_move_only_function` 202110, `__cpp_lib_atomic_wait` 201907, `__cpp_lib_to_chars`
  201611, `__cpp_lib_concepts` 202002, `__cpp_lib_hardware_interference_size` 201703,
  `__cpp_lib_int_pow2` 202002, `__cpp_lib_endian` 201907, `__cpp_lib_ranges_to_container`
  202202, `__cpp_lib_ranges_zip` 202110, `__cpp_lib_ranges_enumerate` 202302,
  `__cpp_lib_bitops` 201907, `__cpp_lib_is_scoped_enum` 202011, `__cpp_if_consteval` 202106,
  `__cpp_explicit_this_parameter` 202110, `__cpp_multidimensional_subscript` 202211,
  `__cpp_designated_initializers` 201707, `__cpp_consteval` 202211, `__cpp_constinit` 201907.
  Không có: `__cpp_lib_flat_map`, `__cpp_lib_mdspan`, `__cpp_lib_constexpr_cmath`,
  `__cpp_lib_start_lifetime_as`, `__cpp_lib_containers_ranges`, `__cpp_lib_format_ranges`.
