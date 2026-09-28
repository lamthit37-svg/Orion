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
  Đã xanh: gtest 1.18.0, benchmark 1.9.5, libsodium 1.0.22, zstd 1.5.7 với `x64-linux-orion` (job
  `§8.3 linux`, `linux-tsan`, `coverage`, `§8.4 clang-tidy`), `arm64-linux-orion` (`§8.3
  linux-arm64`), `x64-windows-orion` (`§8.2 windows` `dev`, `ubsan`), `x64-windows-orion-asan`
  (`§8.2 windows` `asan`) và `arm64-android-orion` (`§8.7 android-arm64`), cùng CI run 36332765141
  (commit `0580b42`). `arm64-ios-orion` chưa build: job iOS chỉ chạy khi bật tay (NGHI-NGO-008).
  boost-beast 1.92.0 và các port Boost nó kéo theo, trong đó boost-asio với tính năng mặc định
  `spawn` kéo boost-context (log vcpkg: `boost-asio[core,deadline-timer,spawn]`): xanh với
  `x64-linux-orion`, `arm64-linux-orion`, `x64-windows-orion`, `x64-windows-orion-asan` ở CI run
  36374741449 (commit `3d7cb02`). Trước đó, CI run 36372839028 (commit `68c8b06`) đỏ ở
  `arm64-linux-orion`: boost-context build assembly x86_64 trên máy arm64, vì toolchain chainload
  không đặt `CMAKE_SYSTEM_PROCESSOR` cho port; `3d7cb02` sửa ở `cmake/toolchains/linux-clang.cmake`.

### NGHI-NGO-002 — Thư viện ngoài build được khi tắt exception và RTTI

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi thư viện ở ARCH §4 dùng được từ code build với `-fno-exceptions -fno-rtti`
  (MSVC: `/EHs-c- /GR-`). Riêng Boost.Beast và Boost.Asio: hoặc dùng được với
  `BOOST_NO_EXCEPTIONS`, hoặc `server/lib/http` phải là module bật exception và bắt hết ở ranh
  giới (CLAUDE.md X.3).
- **Lý do nghi:** ARCH §9. Boost.Asio ném exception ở nhiều API không có overload `error_code`.
- **Cách kiểm:** build `server/lib/http` với `BOOST_NO_EXCEPTIONS` và một hàm
  `boost::throw_exception` gọi `ORION_VERIFY`; chạy test HTTP dưới ASan.
- **Trạng thái:** mở; đóng từng phần theo thư viện. libsodium, zstd, libpq là thư viện C;
  simdjson dùng API mã lỗi (NGHI-NGO-003). Boost.Beast, Boost.Asio dùng được với
  `BOOST_NO_EXCEPTIONS`, không cần module bật exception; đã đo cục bộ với Boost 1.83 của Ubuntu và
  clang 20.1.2: `server_http` build với `-fno-exceptions -fno-rtti` (Boost tự bật
  `BOOST_NO_EXCEPTIONS`, `BOOST_NO_RTTI`) và định nghĩa `boost::throw_exception` bằng
  `ORION_VERIFY` (`detail/boost_throw.cpp`); 49 test của `server_http`, trong đó 18 test chạy
  server thật qua loopback, xanh 10 lần liền ở mỗi preset `local`, `local-asan`, `local-ubsan`,
  `local-tsan`. Với Boost 1.92 của vcpkg: CI run 36374741449 (commit `3d7cb02`) xanh ở mọi job —
  MSVC 14.51.36231 (preset `dev`, `asan`) và clang-cl (preset `ubsan`), mỗi job 596/596 test, có
  49 test của `server_http`; clang 20 Linux x64 (`linux`, `linux-tsan`, `coverage`, clang-tidy) và
  Linux arm64 (`linux-arm64`, 596/596). Phần Boost vì vậy đã đo; các thư viện khác của ARCH §4 đo
  khi được thêm.

### NGHI-NGO-004 — clang và TSan trong WSL2

- **Mở:** 2026-09-27
- **Khẳng định:** Preset `linux-tsan` chạy được trong WSL2 trên máy dev Windows.
- **Lý do nghi:** Hiến pháp II.2 chỉ nói TSan không có trên clang-cl. Đường Linux đã đo trên
  container (NGHI-NGO-018), nhưng WSL2 có kernel và cấu hình ASLR riêng, mà runtime của TSan phụ
  thuộc bố cục bộ nhớ ảo do kernel quyết định.
- **Cách kiểm:** trong WSL2: `cmake --workflow --preset linux-tsan`; nếu TSan báo
  `unexpected memory mapping` thì ghi giá trị `sysctl vm.mmap_rnd_bits`.
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
- **Trạng thái:** mở. Đã đo từng phần trên CI (run 36314444059, commit `5ad2785`):
  - toolchain 1, MSVC 14.51.36231 (Visual Studio 18.10.1, runner `windows-2025`): mọi
    `static_assert` của `features.cpp` xanh ở preset `dev` và `asan`;
  - toolchain 3: xanh (NGHI-NGO-019);
  - toolchain 4, clang của NDK 29.0.14206865 (API mặc định 21): chỉ `__cpp_lib_to_chars` vắng,
    vì libc++ chưa đủ bản số thực; `features.cpp` thay assert đó bằng biên dịch thử bản số nguyên;
  - toolchain 2, clang-cl của LLVM cài sẵn trên runner `windows-2025`: xanh từ run 36314987582
    (commit `eb71d84`), sau khi bỏ `/fp:precise` cho clang-cl;
  - toolchain 5: chưa chạy (NGHI-NGO-008).

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

### NGHI-NGO-023 — UBSan của clang-cl có runtime chạy được

- **Mở:** 2026-09-27
- **Khẳng định:** Preset `ubsan` (clang-cl) link được runtime UBSan và in chẩn đoán có vị trí
  nguồn, thay vì chỉ bẫy.
- **Lý do nghi:** CMake gọi thẳng linker của MSVC với clang-cl, nên runtime của sanitizer không tự
  được thêm như với driver GNU. Tạm thời `orion_flags.cmake` dùng `-fsanitize-trap=undefined`: vẫn
  bắt mọi UB mà UBSan kiểm, nhưng chỉ dừng bằng lệnh bẫy, không in chẩn đoán.
- **Cách kiểm:** job Windows của CI build preset `ubsan` với runtime
  (`clang_rt.ubsan_standalone-x86_64.lib`) và chạy một test cố ý tràn số có dấu.
- **Trạng thái:** mở.

### NGHI-NGO-024 — MSVC STL với `_HAS_EXCEPTIONS=0` sạch dưới `/W4 /WX`

- **Mở:** 2026-09-27
- **Khẳng định:** Với `/EHs-c- /GR- _HAS_EXCEPTIONS=0`, các header chuẩn dự án dùng biên dịch
  không warning dưới `/W4 /WX /permissive-`.
- **Lý do nghi:** Tắt exception trên MSVC không phải cấu hình mặc định; STL có thể phát C4530 hoặc
  warning khác khi `_HAS_EXCEPTIONS` không khớp.
- **Cách kiểm:** job Windows của CI build preset `dev` và `asan`.
- **Trạng thái:** mở.

### NGHI-NGO-026 — Test xanh với đúng bản dependency ghim trong vcpkg

- **Mở:** 2026-09-27
- **Khẳng định:** Mọi test xanh khi build với đúng bản port ghim qua baseline vcpkg (gtest 1.18.0,
  benchmark 1.9.5, libsodium 1.0.22, zstd 1.5.7, và các port thêm sau), không chỉ với bản của gói
  hệ điều hành.
- **Lý do nghi:** Container dựng không tải được nguồn port vcpkg: proxy GitHub của phiên chỉ cho
  git đọc repo công khai và trả 403 cho `github.com/<repo>/archive/*.tar.gz`. Build cục bộ vì vậy
  dùng gói Ubuntu 24.04 (gtest 1.14.0, benchmark 1.8.3, libsodium 1.0.18, zstd 1.5.5) qua
  `CMakeUserPresets.json` riêng máy, cùng toolchain clang 20 và cùng cờ. Chỉ CI trên GitHub dùng
  đúng bản ghim. Với libsodium, port còn build bằng autotools trên Linux và Android, msbuild trên
  Windows (ADR 0011), là đường build chưa chạy lần nào.
- **Cách kiểm:** mỗi commit, đọc kết quả job `§8.3 linux` và `§8.2 windows` của CI; chúng build
  bằng vcpkg ở baseline của `vcpkg.json`.
- **Trạng thái:** mở; đóng từng dependency khi CI xanh với nó. Đã xanh: gtest 1.18.0 và
  benchmark 1.9.5, CI run 36317622896 (commit `406cd3a`): job `§8.3 linux`, `linux-tsan`,
  `coverage`, `§8.2 windows` ở cả ba preset `dev`, `asan`, `ubsan`, mỗi job 63/63 test. libsodium
  1.0.22: CI run 36327515893 (commit `02c21f3`) xanh ở mọi job — port build bằng autotools trên
  Linux x64, Linux arm64 và Android, bằng msbuild trên Windows; test của `engine/crypto`, gồm vector
  RFC 8032, 7748, 8439 và chuỗi Argon2id của argon2-cffi, xanh ở `linux`, `linux-tsan`, `coverage`,
  `linux-arm64` và `windows` `dev`, `asan`, `ubsan`; job fuzz (run 36327518613) xanh. zstd 1.5.7:
  CI run 36332765141 (commit `0580b42`) xanh ở mọi job — port build bằng CMake cho cả năm triplet
  dưới đây; test pak của `engine/io` (đọc entry Zstd, có dictionary) xanh ở `linux`, `linux-tsan`,
  `coverage`, `linux-arm64` và `windows` `dev`, `asan`, `ubsan`; clang-tidy chạy với header của
  1.5.7 (khác 1.5.5 ở chỗ khai báo `ZSTD_getErrorCode`, commit `0580b42`).
  simdjson 4.6.11 (ADR 0014): CI run 36366955171 (commit `a182cd2`) xanh ở mọi job — port build
  từ mã nguồn bằng CMake của simdjson cho các job Linux x64, Linux arm64 và Windows (Android, iOS
  không cài, `vcpkg.json`); test của `server_http` và replay corpus của `json_document` xanh ở
  `linux`, `linux-tsan`, `coverage`, `linux-arm64` và `windows` `dev`, `asan`, `ubsan` (job `dev`:
  559/559 test, có 13 test của `server_http`); clang-tidy chạy với header của 4.6.11. Cục bộ không
  dùng gói Ubuntu (3.6.4 không biên dịch với clang 20) mà hai tệp single-header của đúng tag
  `v4.6.11`, build tĩnh không exception.
  boost-beast 1.92.0 (ADR 0015): CI run 36374741449 (commit `3d7cb02`) xanh ở mọi job — Boost chỉ
  header, cộng boost-context build từ mã nguồn; test của `server_http` (49) và replay corpus của
  `http_request` xanh ở `linux`, `linux-tsan`, `coverage`, `linux-arm64` và `windows` `dev`,
  `asan`, `ubsan`; clang-tidy chạy với header của 1.92.0 và bắt được `basic_fields::contains`, thứ
  1.83 không có (commit `3d7cb02`). Commit thêm server (`68c8b06`, CI run 36372839028) đỏ ở ba chỗ:
  toolchain arm64 (NGHI-NGO-001), test ghi quá hạn trên Windows, phát hiện clang-tidy đó; `3d7cb02`
  sửa cả ba. Cục bộ dùng `libboost1.83-dev` của Ubuntu; hành vi của Beast mà server dựa vào đã đo
  trên 1.83 và ghi ở ADR 0015, và test giữ nó xanh trên cả 1.92.

### NGHI-NGO-028 — Bit kết quả của `engine/math` giống nhau trên arm64

- **Mở:** 2026-09-27
- **Khẳng định:** `engine_math_tests` xanh trên Android arm64 (NDK 29, bionic) và trên Apple arm64
  (Apple clang), gồm golden test `Trig.GoldenBitsAreIdenticalOnEveryToolchain` và bảng làm tròn
  đúng `TrigReference.FaithfullyRoundedOnEveryCase`.
- **Lý do nghi:** CLAUDE.md X.11. Các hàm chỉ dùng phép IEEE làm tròn đúng và `sqrt`, `floor`,
  `fmod` của thư viện C, vốn cho kết quả duy nhất theo chuẩn, nhưng CI chỉ chạy test trên x64:
  job Android chỉ build, job iOS chỉ chạy khi bật tay và cũng chỉ build. Khác biệt có thể đến từ
  chế độ flush-to-zero, từ `fmod` của bionic hay libm của Apple, hoặc từ compiler hợp nhất FMA.
- **Cách kiểm:** đẩy `engine_math_tests` và `engine/math/tests/data/` lên thiết bị hoặc emulator
  Android arm64 bằng `adb` rồi chạy; trên Mac arm64 chạy preset tương ứng của Apple clang.
- **Trạng thái:** mở cho bionic (NDK) và Apple clang. Đã đo, cả hai test xanh với cùng hằng
  golden: clang 20 trên Linux x64 (local, ASan, UBSan, TSan, coverage); CI run 36322188292 (commit
  `fe1c511`) trên MSVC 14.51.36231 (preset `dev`, `asan`) và clang-cl (preset `ubsan`), mỗi job
  102/102 test; CI run 36323827016 (commit `9a51f32`) trên Linux arm64 (runner
  `ubuntu-24.04-arm`, clang 20, glibc), 122/122 test. Kiến trúc arm64 vì vậy đã đo; phần còn lại
  là libm của bionic và Apple.

### NGHI-NGO-029 — SIMD viết tay cho `engine/math`

- **Mở:** 2026-09-27
- **Khẳng định:** Hot path dùng `engine/math` theo lô (skinning, culling, AOI) cần SIMD viết tay
  (SSE4.2 trên x64, NEON trên ARM64, ARCH §4.1) mới đạt ngân sách X.8; bản vô hướng hiện tại không
  đủ.
- **Lý do nghi:** ARCH §4.1 chọn SIMD, nhưng X.8 cấm tối ưu vi mô khi chưa có số đo, và chưa có hot
  path thật để đo. Hiện clang build với `-march=x86-64-v2` (có SSE4.2) nên code vô hướng đã được
  tự vector hoá khi compiler thấy lợi; MSVC và clang-cl chưa đặt `/arch:SSE4.2`.
- **Cách kiểm:** khi có hot path đầu tiên dùng `engine/math` theo lô, benchmark bản vô hướng và
  bản SIMD ở preset `profile` trên máy tham chiếu (ADR 0005); bản SIMD phải giữ đúng thứ tự phép
  tính để golden test của `engine/math` không đổi.
- **Trạng thái:** mở.

### NGHI-NGO-031 — Sanitizer thấy lỗi bộ nhớ bên trong libsodium

- **Mở:** 2026-09-27
- **Khẳng định:** Fuzz và các preset sanitizer phát hiện được lỗi bộ nhớ hay UB bên trong
  libsodium khi nó xử lý dữ liệu từ ngoài (chuỗi băm mật khẩu, gói AEAD, chữ ký, khoá công khai).
- **Lý do nghi:** Triplet `x64-linux-orion` build dependency không có cờ sanitizer, nên ASan và
  UBSan chỉ instrument code của dự án; libsodium đọc quá vùng nhớ chỉ lộ ra nếu gây SEGV. Fuzz
  target `crypto_password_hash` có gọi bộ giải mã PHC của libsodium, nhưng ở dạng chưa instrument.
- **Cách kiểm:** thêm triplet build dependency với `-fsanitize=address,undefined` cho preset
  `linux-fuzz` (như `x64-windows-orion-asan` cho MSVC), rồi chạy lại fuzz đêm.
- **Trạng thái:** mở. Cùng giới hạn đó áp cho zstd và cho TSan: preset `linux-tsan` không thấy
  truy cập bên trong zstd; điều code dựa vào ở đó được đo riêng ở NGHI-NGO-033.

### NGHI-NGO-032 — Tệp của `engine/io` trên Apple

- **Mở:** 2026-09-27
- **Khẳng định:** `engine/io/apple/native_file.cpp` build được với Apple clang và cho kết quả như
  bản Linux ở `engine_io_tests`; `fcntl(F_FULLFSYNC)` đẩy dữ liệu xuống thiết bị lưu trữ mạnh hơn
  `fsync` trên iOS và macOS, và hệ thống tệp không nhận lệnh này thì trả lỗi để code lùi về `fsync`.
- **Lý do nghi:** CI chỉ build iOS khi bật tay (NGHI-NGO-008) và chưa chạy test trên Apple; lý do
  dùng F_FULLFSYNC lấy từ tài liệu của Apple mà phiên dựng không đọc lại được.
- **Cách kiểm:** chạy `engine_io_tests` trên máy Mac (preset của Apple clang); đọc trang man
  `fsync(2)` và `fcntl(2)` của macOS.
- **Trạng thái:** mở.

---

### NGHI-NGO-034 — Golden replay khớp trên NDK arm64 và Apple arm64

- **Mở:** 2026-09-27
- **Khẳng định:** `replay_tests` (`tests/replay/`) xanh với đúng các tệp `tests/replay/golden/*.txt`
  của repo trên Android arm64 (NDK, bionic) và Apple arm64 (Apple clang), như CLAUDE.md X.11 đòi.
- **Lý do nghi:** Mô phỏng chỉ dùng + − × ÷, căn và `floor` của `engine/math` trên f64, không FMA,
  nên kết quả là duy nhất theo IEEE 754; nhưng CI chỉ build Android, iOS chỉ chạy khi bật tay
  (NGHI-NGO-008), nên hai nền tảng này chưa chạy test lần nào.
- **Cách kiểm:** build preset `android-arm64`, đẩy `replay_tests` và `tests/replay/golden/` lên
  thiết bị hay emulator arm64 bằng `adb push`, rồi chạy với `ORION_REPLAY_GOLDEN_DIR` trỏ tới thư
  mục golden đã đẩy; trên Mac arm64 chạy `ctest -R tests/replay` ở preset của Apple clang.
- **Trạng thái:** mở, chỉ còn NDK arm64 và Apple arm64. Đã khớp: clang 20 trên Linux x64 ở preset
  local, local-asan, local-ubsan, local-tsan và local-coverage (cùng tệp golden, dù mức tối ưu và
  instrument khác nhau); và trong CI run 36357932698 (commit 925dc95): MSVC 14.51 x64 ở preset dev
  và asan, clang-cl ở preset ubsan, clang 20 Linux x64 ở linux, linux-tsan, linux-coverage, clang 20
  Linux arm64 ở linux-arm64 — log job `§8.2 windows: dev` có đủ bốn test `tests/replay` qua.

### NGHI-NGO-035 — server_db đúng với libpq 18.4 của vcpkg và PostgreSQL 18

- **Mở:** 2026-09-27
- **Khẳng định:** Test của `game/server/lib/db` xanh khi link libpq 18.4 bản vcpkg (tĩnh, không
  OpenSSL) và chạy trên PostgreSQL 18, kể cả xác thực SCRAM-SHA-256 bằng mật khẩu; và trên Windows
  (MSVC, clang-cl) khi có PostgreSQL để test.
- **Lý do nghi:** Container dựng không tải được nguồn port vcpkg (NGHI-NGO-026), nên mọi lần chạy
  cục bộ dùng libpq 16.15 của Ubuntu (có OpenSSL) với PostgreSQL 16.15. Bản không OpenSSL tự băm
  SCRAM và lấy số ngẫu nhiên từ hệ điều hành, đường code chưa chạy lần nào ở đây. Job Windows của CI
  không có PostgreSQL, nên test cần DB ở đó tự bỏ qua.
- **Cách kiểm:** job `§8.3 linux`, `linux-tsan`, `linux-arm64` và `coverage` của CI chạy
  `server_db_tests` với service `postgres:18.6` qua mật khẩu (`ORION_REQUIRE_POSTGRES=1`, nên bỏ
  qua là đỏ). Windows: trên máy dev, PostgreSQL trong WSL2 (ARCH §1), đặt `ORION_TEST_POSTGRES` rồi
  `ctest --preset dev -R game/server/lib/db`.
- **Trạng thái:** mở, chỉ còn phần Windows. Phần Linux đã có bằng chứng: CI run 36359751549 (commit
  6ee8195), bốn job `§8.3 linux: linux`, `linux: linux-tsan`, `linux-arm64` và `coverage: llvm-cov`
  xanh, link libpq 18.4 bản vcpkg (baseline ghim, ADR 0013), chạy trên service `postgres:18.6` (log
  của service: `starting PostgreSQL 18.6 (Debian 18.6-1.pgdg13+2)`). Kết nối TCP đi qua
  SCRAM-SHA-256: entrypoint của image (docker-library/postgres, `18/trixie/docker-entrypoint.sh`,
  hàm `pg_setup_hba_conf`) ghi `host all all all` với phương thức bằng `password_encryption`, mặc
  định `scram-sha-256` từ PostgreSQL 14. Job `linux`: 492 test qua, chỉ `Environment.ValuesAreUtf8`
  bỏ qua theo thiết kế (nó chạy ở test `environment-utf8`); với `ORION_REQUIRE_POSTGRES=1`, test cần
  DB không bỏ qua được. Log của service có đúng các lỗi mà test gây ra, và `FATAL: connection to
  client lost` cho câu `pg_sleep(30)` bị bỏ giữa chừng, khoảng 1 s sau
  (`client_connection_check_interval`). Cục bộ (libpq 16.15, PostgreSQL 16.15): 41 test của
  server_db và 18 test của `tools/migrate` (12 cần DB) xanh, cả với một vai trò không phải superuser
  đăng nhập bằng SCRAM. Windows: job `§8.2 windows` (MSVC, clang-cl) build libpq bản vcpkg và chạy
  các test không cần DB; test cần DB chưa chạy.

### NGHI-NGO-036 — Tiến trình server trên Windows dừng êm khi đóng console

- **Mở:** 2026-09-28
- **Khẳng định:** Trên Windows, tiến trình server nhận Ctrl+C, Ctrl+Break hay đóng cửa sổ console
  thì `StopSignal::wait` trả về và main dừng êm, ghi hết log; với đóng console, việc đó xong trong
  thời gian chờ của hệ điều hành.
- **Lý do nghi:** Test của `game/server/lib/service` chỉ kiểm event có tên mà handler console báo
  (`tests/win/`), không gửi sự kiện console thật: `GenerateConsoleCtrlEvent` đi tới mọi tiến trình
  dùng chung console, kể cả ctest. Hành vi của hệ điều hành lấy từ tài liệu HandlerRoutine của
  Microsoft (tệp nguồn `docs/handlerroutine.md` của repo MicrosoftDocs/Console-Docs), chưa chạy lần
  nào: handler chạy trên một luồng mới hệ điều hành tạo trong tiến trình; với đóng console, trả
  TRUE thì hệ điều hành kết thúc tiến trình; thời gian chờ của đóng console là
  `SPI_GETHUNGAPPTIMEOUT`, mặc định 5000 ms; Ctrl+C, Ctrl+Break không có thời gian chờ.
- **Cách kiểm:** trên máy dev Windows, khi có tiến trình server đầu tiên: chạy nó trong một cửa sổ
  console riêng, bấm Ctrl+C, rồi đọc log (có dòng dừng êm, mã thoát 0); chạy lại, đóng cửa sổ, rồi
  đọc tệp log mà stdout được chuyển vào (có dòng dừng êm trước khi tiến trình kết thúc).
- **Trạng thái:** mở.

## Đã đóng

### NGHI-NGO-003 — simdjson: API mã lỗi đủ dùng khi tắt exception

- **Mở:** 2026-09-27
- **Khẳng định:** Parser request HTTP viết được hoàn toàn bằng API `simdjson_result`/`error_code`
  với `SIMDJSON_EXCEPTIONS=0`.
- **Lý do nghi:** ARCH §9. Một số tiện ích của simdjson chỉ có bản ném exception.
- **Cách kiểm:** build parser request với `-DSIMDJSON_EXCEPTIONS=0 -fno-exceptions`; fuzz 10 phút.
- **Trạng thái:** đóng 2026-09-28 cho simdjson 4.6.11; đo lại khi đổi phiên bản (ADR 0014).
- **Bằng chứng:** mọi lời gọi simdjson của dự án nằm trong `game/server/lib/http/json.cpp`
  (`check_layers.py` chặn `<simdjson.h>` ở nơi khác); parser request của từng endpoint chỉ thấy
  `json.hpp`. Tệp đó biên dịch với `-fno-exceptions -fno-rtti -DSIMDJSON_EXCEPTIONS=0` (dòng lệnh
  trong `compile_commands.json` của preset `local`), clang 20.1.2, simdjson 4.6.11 (hai tệp
  single-header của tag `v4.6.11`). Với `SIMDJSON_EXCEPTIONS=0`, `simdjson.h` không khai báo bản
  ném exception nào (chúng nằm trong `#if SIMDJSON_EXCEPTIONS`), nên dùng nhầm là lỗi biên dịch chứ
  không phải lỗi lúc chạy; header tự đặt macro về 0 khi không có `__cpp_exceptions` hay
  `_CPPUNWIND`, nên MSVC với `/EHs-c-` đi cùng đường. API đã dùng, đủ cho mọi loại giá trị của RFC
  8259: `dom::parser` (`allocate`, `parse`), `dom::element` (`type`, `get_int64`, `get_uint64`,
  `get_double`, `get_string`, `get_bool`, `get_array`, `get_object`), iterator của `dom::array` và
  `dom::object`. 13 test của `server_http` xanh ở `local`, `local-asan`, `local-ubsan`,
  `local-tsan`, `local-coverage`. Fuzz `json_document` (preset `local-fuzz`: libFuzzer, ASan, UBSan)
  trên đúng code của commit thêm module: 611 giây, 23 692 042 lượt (khoảng 38 800 lượt mỗi
  giây), RSS đỉnh 443 MB, không crash.

### NGHI-NGO-005 — CMake phát `/external:I` cho include SYSTEM với MSVC

- **Mở:** 2026-09-27
- **Khẳng định:** Với MSVC, target IMPORTED của vcpkg được include bằng `/external:I`, nên
  `/external:W0` chặn được warning từ header ngoài mà không phải hạ `/W4 /WX`.
- **Lý do nghi:** ARCH §9. Chưa có máy Windows trong phiên dựng.
- **Cách kiểm:** job Windows của CI in `compile_commands.json` của preset `dev`; tìm `/external:I`
  trên dòng biên dịch một file include libsodium.
- **Trạng thái:** đóng 2026-09-27.
- **Bằng chứng:** CI run 36315453481, job `§8.2 windows: dev` (MSVC 14.51.36231): dòng biên dịch
  `narrow_test.cpp` có `-external:ID:\a\Orion\Orion\out\build\dev\vcpkg_installed\x64-windows-orion\include
  -external:W0` cho include của GoogleTest, cạnh `/W4 /WX`. Header của vcpkg vì vậy không bắt dự án
  hạ cờ warning.

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
  `__cpp_nontype_template_args` chỉ là 201411 (chưa đủ tham số template kiểu lớp theo macro), đo
  khi build `toolchain_features` lần đầu; dự án không dùng tính năng này.

### NGHI-NGO-025 — ASan của MSVC với thư viện tĩnh vcpkg không bật ASan

- **Mở:** 2026-09-27
- **Khẳng định:** Preset `asan` link được code dự án build với `/fsanitize=address` cùng thư viện
  tĩnh của vcpkg, không lỗi `detect_mismatch` về annotation của `std::vector` và `std::string`.
- **Lý do nghi:** STL của MSVC ghi dấu annotation của container vào object file; trộn object có và
  không có ASan có thể bị linker từ chối.
- **Cách kiểm:** job Windows của CI build và chạy test preset `asan` khi đã có dependency đầu tiên.
- **Trạng thái:** đóng 2026-09-27. Khẳng định ban đầu (dùng chung triplet `x64-windows-orion`) sai;
  cách đang dùng là triplet `x64-windows-orion-asan`.
- **Bằng chứng:** CI run 36316870461 (MSVC 14.51.36231) với triplet `x64-windows-orion` báo
  `LNK2038: mismatch detected for 'annotate_string' / 'annotate_vector' / 'annotate_optional'`
  giữa `gtest.lib` và test của dự án. Triplet `x64-windows-orion-asan` build dependency với
  `/D_ANNOTATE_STL`; theo `stl/inc/__msvc_sanitizer_annotate_container.hpp` của microsoft/STL
  (commit `f023531`), macro này chèn code annotation mà không ghi `detect_mismatch` nào. CI run
  36317622896, job `§8.2 windows: asan`: `cmake --preset asan`, build và `ctest --preset asan`
  xanh, `100% tests passed out of 63`.

### NGHI-NGO-027 — clang-tidy 20.1.0 áp dụng `HeaderFilterRegex` của `.clang-tidy`

- **Mở:** 2026-09-27
- **Khẳng định:** Với bản ghim clang-tidy 20.1.0, khoá `HeaderFilterRegex` trong `.clang-tidy` được
  áp dụng, nên `tools/run_tidy.py` báo cả phát hiện trong header của dự án.
- **Lý do nghi:** header của `engine/math` có nhiều biểu thức mà
  `readability-math-missing-parentheses` phải báo, nhưng run_tidy chỉ báo ở tệp `.cpp`.
- **Cách kiểm:** chạy `clang-tidy -p out/build/local
  --checks='-*,readability-math-missing-parentheses' engine/math/quat.cpp` không có và có
  `--header-filter` với đúng regex của `.clang-tidy`; `--dump-config` để xem khoá đã được đọc.
- **Trạng thái:** đóng 2026-09-27. Khẳng định sai.
- **Bằng chứng:** `--dump-config` in đúng `HeaderFilterRegex: '.*/(engine|game|tools|tests)/.*'`,
  nhưng lệnh không có `--header-filter` báo 0 phát hiện trong `quat.hpp` và in
  `Suppressed 4777 warnings (4777 in non-user code)`; cùng lệnh với
  `--header-filter='.*/(engine|game|tools|tests)/.*'` báo 20 phát hiện trong `quat.hpp`. Từ nay
  `run_tidy.py` đọc regex trong `.clang-tidy` và truyền lại qua `--header-filter`; lần chạy đầu
  tìm ra 13 phát hiện đã bị giấu trong header của `engine/core`, sửa ở commit `c9b75f2`.

### NGHI-NGO-030 — Job system đúng trên mô hình bộ nhớ yếu của arm64

- **Mở:** 2026-09-27
- **Khẳng định:** Deque Chase–Lev, eventcount và `JobCounter` của `engine/jobs` không làm mất, không
  chạy lặp job và không treo trên arm64 (Android, Apple Silicon), nơi CPU được phép sắp lại thứ tự
  truy cập bộ nhớ nhiều hơn x64.
- **Lý do nghi:** Thứ tự bộ nhớ theo bản đã chứng minh của Lê và cộng sự (PPoPP 2013), có lý do
  từng dòng, và TSan xanh. Nhưng x64 giữ thứ tự ghi (TSO) nên che phần lớn lỗi thứ tự, TSan không
  mô hình hàng rào (fence), và CI chưa chạy test trên arm64.
- **Cách kiểm:** chạy `engine_jobs_tests --gtest_repeat=1000` trên thiết bị Android arm64 và trên
  Mac Apple Silicon, hoặc trên runner Linux arm64 của CI.
- **Trạng thái:** đóng 2026-09-27 cho Linux arm64; Android và Apple Silicon chưa chạy trên thiết
  bị thật (cách kiểm cho phép runner Linux arm64 thay cho chúng).
- **Bằng chứng:** trên x64: 300 lượt lặp ở preset `local`, 50 lượt dưới TSan, xanh. Trên Linux
  arm64 (runner `ubuntu-24.04-arm`): một lượt ở CI run 36323827016; rồi bước "Lặp test job system"
  của job `§8.3 linux-arm64` chạy `engine_jobs_tests --gtest_repeat=200 --gtest_filter=-*DeathTest*`
  (18 test mỗi lượt) và xanh ở CI run 36324283492, 36327515893, 36328147965, 36328657911,
  36330676036, 36332192303, 36332765141 và 36334195631: tổng 1600 lượt, không lượt nào hỏng hay
  treo. Log của run 36334195631 có đúng 200 dòng `[  PASSED  ] 18 tests.` và không dòng FAILED nào.

### NGHI-NGO-033 — Nhiều luồng dùng chung một `ZSTD_DDict`

- **Mở:** 2026-09-27
- **Khẳng định:** `PakReader` dựng mỗi dictionary của pak thành một `ZSTD_DDict` khi mở, rồi mọi
  luồng đọc, mỗi luồng một `ZSTD_DCtx`, dùng chung nó qua `ZSTD_decompress_usingDDict` mà không
  khoá.
- **Lý do nghi:** `zstd.h` của v1.5.7 nói rõ `ZSTD_CDict` dùng chung giữa các luồng được, còn với
  `ZSTD_DDict` thì không nói gì. TSan của preset `linux-tsan` không thấy truy cập bên trong zstd vì
  dependency không được instrument (NGHI-NGO-031), nên `PakTest.ConcurrentReadersWithSeparateContexts`
  chỉ kiểm phần code của dự án.
- **Cách kiểm:** biên dịch mã nguồn zstd ở tag `v1.5.7` (repo `facebook/zstd`, bản ghim ở ADR 0012)
  cùng một harness với `-fsanitize=thread` và `-DZSTD_DISABLE_ASM`, để mọi đường giải nén là code C
  có instrument. 8 luồng, mỗi luồng một `ZSTD_DCtx`, giải nén 48 frame × 60 vòng qua cùng một
  `ZSTD_DDict`; làm với một dictionary chỉ có nội dung và một dictionary huấn luyện bằng
  `ZDICT_trainFromBuffer` (có bảng entropy mà DCtx trỏ thẳng vào). Đối chứng: cùng harness nhưng
  các luồng dùng chung một `ZSTD_DCtx`.
- **Trạng thái:** đóng 2026-09-27 cho zstd v1.5.7; đo lại khi đổi phiên bản zstd.
- **Bằng chứng:** mỗi loại dictionary 23 040 lần giải nén, 0 kết quả sai, 0 cảnh báo TSan. Đối
  chứng: 103 cảnh báo `data race` trong `lib/decompress/zstd_decompress.c` (`ZSTD_decompressBegin`,
  `ZSTD_decompressBegin_usingDDict`), nên TSan của harness thấy được truy cập bên trong zstd.
