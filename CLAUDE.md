# Orion — CLAUDE.md

File này là **mục X** của hiến pháp máy cho dự án Orion (repo `EngineGAME/`). Hiến pháp vẫn áp
dụng đầy đủ. File này chỉ thêm luật riêng của Orion và trỏ tới hiến pháp bằng số, không chép lại.
Chỗ nào trùng nhau thì hiến pháp thắng.

Bố cục, công nghệ và ba vùng DEV, CHẠY, CHƠI nằm ở `docs/ARCHITECTURE.md`, viết tắt là ARCH. ARCH
là bản đồ; file này là luật viết code.

Mỗi luật kết thúc bằng một nhãn cho biết ai bắt nó:

- `[cổng]` — tool chặn build hoặc CI (compiler, clang-tidy, script kiểm, job CI);
- `[test]` — test, fuzz hoặc benchmark bắt;
- `[review]` — không tool nào bắt, nên phải tự soát trước khi nói xong.

---

## X.0 Thứ tự ưu tiên

Khi hai yêu cầu đụng nhau, mục đứng trước thắng:

1. **Đúng** — không UB, không race, không mất dữ liệu người chơi.
2. **An toàn** — ranh giới tin cậy ở X.9 giữ nguyên.
3. **Tất định** — `game/shared` cho cùng kết quả trên mọi toolchain (X.11).
4. **Đo được** — mọi khẳng định có số đo, hoặc có mục trong sổ nghi ngờ (X.6).
5. **Nhanh** — nằm trong ngân sách ở X.8.
6. **Gọn** — ít code, ít tầng trừu tượng.

Muốn đổi một mục cao lấy một mục thấp hơn thì phải có ADR.

---

## X.1 Toolchain, cờ, cổng

- Windows dùng MSVC qua cổng `dev.bat` (hiến pháp II), cho client, tool và bản debug của server.
  `[cổng]`
- Dự án này thêm toolchain thứ hai: clang với libstdc++ trên Linux (WSL2 và CI), cho server
  production, `game/shared`, tool, fuzz và TSan. Mobile thêm clang của NDK và Apple clang. Đủ năm
  toolchain được liệt kê ở X.12. `[cổng]`
- Warning là lỗi trên mọi toolchain. Cờ sàn nằm trong `cmake/orion_flags.cmake`:
  - MSVC: `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /external:W0`;
  - clang: `-Wall -Wextra -Wpedantic -Werror`, `-Wconversion -Wsign-conversion -Wshadow`,
    `-Wnon-virtual-dtor -Wold-style-cast -Wimplicit-fallthrough`.

  `[cổng]`
- Header của thư viện ngoài được include dạng SYSTEM, để warning của chúng không bắt mình hạ cờ.
  `[cổng]`
- Không tắt warning để build xanh. Tắt một warning cụ thể thì chỉ quanh đúng dòng cần (push/pop
  hoặc `suppress`), kèm lý do ngay tại chỗ. `[review]`
- clang-tidy chạy trong CI với `.clang-tidy` của repo; phát hiện nào của nó cũng chặn merge.
  `[cổng]`
- Cổng kiểm gồm `orion_check_style` (hiến pháp V.5), `tools/check_tracked.py` và
  `tools/check_layers.py`. `check_layers.py` bắt năm lỗi:
  - include đi ngược tầng (ARCH §3);
  - include vào `detail/` của module khác;
  - macro nền tảng dùng ngoài thư mục nền tảng (X.2);
  - header thư viện ngoài dùng ngoài module bọc nó (X.2);
  - header bị cấm trong `game/shared` (X.11).

  `[cổng]`
- `.clang-format` là bất biến (hiến pháp V.4). Không viết code vòng vèo để né format. `[cổng]`

---

## X.2 Bố cục, module, tầng

- Cây thư mục ở ARCH §2 là luật. Thêm thư mục cấp cao, đổi tầng hay chuyển module sang tầng khác
  đều cần ADR. `[review]`
- Mỗi module là một thư mục và một target, tạo bằng `orion_add_module()`. Không gọi `add_library`
  tay trong `engine/` và `game/`. `[review]`
- `.hpp` nằm cạnh `.cpp` (hiến pháp IV). Header public đặt ở gốc module; thứ nội bộ đặt trong
  `detail/`. `[cổng]`
- Test của module nằm ở `<module>/tests/`; test liên module nằm ở `tests/`. `[review]`
- Tầng theo ARCH §3. Không có vòng phụ thuộc giữa các module. `[cổng]`
- Code riêng một nền tảng chỉ nằm trong thư mục con `win/`, `linux/`, `android/`, `apple/` của
  module. Macro nền tảng chỉ được định nghĩa trong `engine/core/platform.hpp`. `[cổng]`
- Trong `engine/` và `game/`, thư viện ngoài chỉ được include trong module bọc nó: Jolt trong
  `engine/physics`, API đồ hoạ trong `engine/rhi/<api>/`, libsodium trong `engine/crypto`, libpq
  trong `server/lib/db`, Boost.Beast trong `server/lib/http`, v.v. Header public của module không
  để lộ kiểu của thư viện ngoài. Ngoại lệ duy nhất là EnTT, dùng thẳng
  trong `server/world` và `server/instance` (ARCH §4.5). `tools/` không bị luật này ràng buộc.
  `[cổng]`
- Hàm dài quá 60 dòng hoặc lồng quá 4 cấp thì phải tách. `[cổng]` File dài quá 1000 dòng thì phải
  tách. `[review]`

---

## X.3 Ngôn ngữ và kiểu

- Dùng C++23, nhưng chỉ phần giao đã đo ở X.12. C thuần dùng C11. `[cổng]`
- Tắt exception và RTTI trong `engine/` và `game/`. Lỗi đi qua `std::expected<T, Error>` (X.5).
  `tools/` được dùng exception bên trong, nhưng không để exception thoát qua API. Thư viện ngoài
  nào cần exception thì bọc trong một module riêng build có exception, và bắt hết ở ranh giới.
  `[cổng]`
- Kiểu số dùng alias trong `engine/core/types.hpp`: `i8`…`i64`, `u8`…`u64`, `f32`, `f64`, `usize`.
  `int`, `long`, `unsigned` trần chỉ dùng khi API ngoài bắt buộc. `[review]`
- Chuyển kiểu có thể làm mất giá trị thì đi qua `orion::narrow<T>()`, hàm này assert khi mất giá
  trị. Dữ liệu từ ngoài thì kiểm rồi trả lỗi, không assert. `[cổng]`
- Luôn dùng `enum class`. Enum ghi ra đĩa hay gửi qua mạng phải có kiểu nền và giá trị tường minh,
  và không bao giờ đổi số của một giá trị cũ. `[review]`
- Không có biến toàn cục hay `static` có thể đổi, không có singleton. Trạng thái dùng chung truyền
  qua tham số hoặc đối tượng context. Ngoại lệ: logger và allocator gốc, khởi tạo đúng một lần
  trong `main`. `[review]`
- Kiểu `Error` và `Handle<T>` khai `[[nodiscard]]` ở mức lớp; hàm trả `expected` cũng đánh
  `[[nodiscard]]`. `[review]`
- `const` là mặc định cho tham số, biến cục bộ và hàm thành viên không đổi trạng thái. `[cổng]`
- Chuỗi là UTF-8 ở mọi nơi. Chỉ đổi sang UTF-16 hay kiểu path của hệ điều hành ở ranh giới nền
  tảng. `[review]`
- Chỉ dùng macro khi không còn cách nào khác (assert cần vị trí nguồn, cờ nền tảng). Tên macro là
  chữ hoa, bắt đầu bằng `ORION_`. `[review]`
- Không `using namespace` trong header. Mọi code nằm trong `namespace orion::<module>`. `[review]`
- Cấm đệ quy trong `engine/` và `game/`. `[cổng]`
- Cấm UB có chủ đích: ép kiểu qua `union` (dùng `std::bit_cast`), đọc ngoài biên, tràn số có dấu.
  `reinterpret_cast` chỉ được dùng trong serializer và `engine/rhi`. Không dùng `const_cast` để ghi
  vào đối tượng const. `[cổng]`

---

## X.4 Kiểm thử

- Hành vi mới phải đi kèm test. Sửa bug thì viết test tái hiện trước, thấy nó đỏ, rồi mới sửa.
  `[review]`
- Các bậc test:
  - **unit** — GoogleTest trong `<module>/tests/`, chạy bằng `ctest` ở mọi preset;
  - **replay** — `tests/replay/`: ghi log input, rồi so hash trạng thái từng tick; phải khớp trên
    mọi toolchain (X.11);
  - **fuzz** — mỗi parser nhận dữ liệu từ ngoài (gói tin, pak, manifest, asset cooked, JSON data,
    token, request HTTP) có một target libFuzzer. Corpus nằm ở `tests/fuzz/corpus/`. Crash nào fuzz
    tìm ra đều thành một test unit;
  - **benchmark** — Google Benchmark cho mọi hot path có ngân sách ở X.8;
  - **ảnh vàng** — mỗi pass render có một ảnh chuẩn và sai số cho phép. Chạy trên WARP để không
    phụ thuộc GPU của CI; tier mobile cũng chạy được trên PC nên được test cùng cách;
  - **soak** — `tests/soak/`: đàn bot vào cụm CHẠY.

  `[review]`
- Coverage dòng tối thiểu, đo bằng llvm-cov trong CI Linux: 90% cho `game/shared`, `engine/net`,
  `engine/io` và code sinh protocol; 80% cho phần còn lại của T0 và T1. `[cổng]`
- Test phải tất định: không `sleep`, không dùng đồng hồ thật (tiêm đồng hồ giả), không mạng ngoài
  loopback, seed cố định và được in ra khi test đỏ. `[review]`
- Test chập chờn là bug. Không chạy lại cho qua, không đánh dấu disabled. Mở một mục trong sổ nghi
  ngờ và sửa nó. `[review]`
- Không sửa test cho khớp với code sai. Đổi kỳ vọng của một test thì ghi lý do trong commit.
  `[review]`

---

## X.5 Lỗi, assert, log

- Ba loại lỗi, ba công cụ:
  - lỗi lập trình, tức vi phạm tiền điều kiện nội bộ: `ORION_ASSERT` — bật ở mọi preset trừ `ship`;
  - bất biến mà vỡ thì hỏng bộ nhớ hoặc dữ liệu: `ORION_VERIFY` — bật ở mọi bản, kể cả `ship`; vỡ
    thì crash kèm minidump;
  - lỗi có thể xảy ra lúc chạy (file thiếu, gói hỏng, DB timeout): trả `std::expected<T, Error>`.

  `[review]`
- Dữ liệu từ ngoài không bao giờ đi vào assert; nó được kiểm rồi trả lỗi. "Từ ngoài" gồm: mạng,
  file, DB, script, input người chơi, và asset cooked. `[test]`
- `Error` mang một mã enum và ngữ cảnh ngắn, và không cấp phát trên hot path. `[review]`
- Không nuốt lỗi. Mỗi lỗi được xử lý, trả tiếp lên, hoặc log đúng một lần tại nơi xử lý. Không log
  rồi lại trả tiếp. `[review]`
- Không gọi `.value()` trên `expected` hay `optional`. Kiểm trước, rồi dùng `*` hoặc `->`.
  `[review]`
- Mức log: trace, debug, info, warn, error. Hot path không log, trừ khi có giới hạn tần suất. Log
  của server là JSON lines, kèm `zone`, `tick`, `entity` khi có. `[review]`
- Không log bí mật (mật khẩu, token, khoá). Dữ liệu cá nhân chỉ được log ở dạng đã băm. `[review]`
- Zone crash không được làm hỏng dữ liệu bền: mọi lần ghi bền đều đi qua transaction (X.9).
  `[test]`

---

## X.6 Sổ nghi ngờ — `docs/NGHI-NGO.md`

- Mọi thứ chưa đo là một mục trong sổ, không phải một câu trong comment: hành vi của API, port
  vcpkg, tính năng của toolchain, giả định về hiệu năng. `[review]`
- Mỗi mục có: mã `NGHI-NGO-NNN`, ngày mở, khẳng định cần kiểm, lý do nghi, cách kiểm (lệnh cụ thể),
  trạng thái. Khi đóng, ghi thêm bằng chứng: lệnh đã chạy, kết quả, hoặc link nguồn. `[review]`
- Code dựa trên một giả định chưa kiểm thì ghi mã mục ngay tại chỗ, ví dụ
  `// NGHI-NGO-012: <giả định>`. `[review]`
- Comment và commit không dùng "chắc là", "hình như", "có lẽ". Hoặc đã đo và ghi nguồn, hoặc mở
  một mục. `[review]`
- Đóng mục mà không có bằng chứng là vi phạm. Mục đã đóng không bị xoá, mà chuyển xuống phần "Đã
  đóng". `[review]`

---

## X.7 Bộ nhớ, sở hữu, luồng

- Dùng RAII (hiến pháp III.4). Không có `new`, `delete`, `malloc`, `free` trần ngoài allocator của
  `engine/core`. `[cổng]`
- Sở hữu duy nhất là mặc định. Mỗi lần dùng `shared_ptr` phải có lý do, và nó bị cấm trong mô
  phỏng và hot path. `[review]`
- Qua ranh giới giữa các hệ thống thì dùng handle có generation (`Handle<T>`), không dùng con trỏ.
  Con trỏ thô không bao giờ sống qua một frame hay một tick. `[review]`
- Hot path (mỗi frame, mỗi tick, mỗi gói) không cấp phát heap sau khi đã khởi động xong. Test và
  benchmark đếm số lượt cấp phát, và chặn nếu con số khác 0. `[test]`
- Vùng nhớ liền được truyền bằng `std::span`, không truyền cặp con trỏ và độ dài. `[review]`
- Không `memcpy` struct ra đĩa hay ra mạng. Mọi định dạng đều đi qua serializer có kiểu và thứ tự
  byte tường minh (X.10). `[review]`
- Đối tượng của thư viện ngoài được bọc RAII ngay trong module bọc nó:
  - COM của D3D12: `ComPtr`;
  - Vulkan: lớp sở hữu riêng, cộng hàng đợi huỷ trễ theo frame;
  - metal-cpp: `NS::SharedPtr`, cộng một `NS::AutoreleasePool` cho mỗi frame và mỗi luồng;
  - `PGresult`, `redisReply`: deleter riêng.

  `[review]`
- Mô hình luồng theo ARCH §7. Không tạo `std::thread` ngoài `engine/jobs` và các luồng đã được khai
  ở đó. `[review]`
- Dữ liệu dùng chung giữa các luồng phải có đúng một cách đồng bộ, ghi trong comment của kiểu đó:
  khoá bằng mutex nào, hay chỉ một luồng sở hữu, hay atomic với memory order nào. `[review]`
- Mỗi lần dùng memory order khác `seq_cst` phải có một dòng lý do. `[review]`
- Hot path của mô phỏng không dùng khoá; các luồng trao dữ liệu cho nhau qua hàng đợi. `[review]`
- Code chạy nhiều luồng có test chạy dưới TSan (preset `linux-tsan`), khi TSan đã đo được. `[test]`

---

## X.8 Hiệu năng và ngân sách

Mọi số dưới đây là **giả định** cho tới lần đo đầu tiên (ARCH §9). Đo xong thì sửa số và ghi
nguồn. Máy tham chiếu chốt ở ADR 0005.

| Hạng mục | Ngân sách |
|---|---|
| Tick của một zone | p99 ≤ 10 ms, p99.9 ≤ 16 ms, trong khung 20 ms |
| Tải mỗi zone | 500 người chơi và 5000 NPC trong ngân sách tick ở trên |
| Mạng xuống, mỗi client | trung bình ≤ 16 KB/s; p95 ≤ 32 KB/s khi thấy 100 thực thể |
| Mạng lên, mỗi client | ≤ 4 KB/s |
| Client PC tier cao | 60 fps: luồng chính ≤ 6 ms, luồng render ≤ 6 ms, GPU ≤ 14 ms |
| Client mobile | 30 fps ổn định sau 30 phút chơi, không tụt vì nhiệt |
| Giật hình | không frame nào quá 2 lần ngân sách vì streaming, tạo PSO hay GC của Luau |
| Bộ nhớ mobile | theo lớp thiết bị ở ADR 0005 |

- Mỗi ngân sách có một benchmark hoặc một metric telemetry đo nó. Thay đổi nào làm vượt ngân sách,
  hoặc chậm hơn baseline quá 5%, đều bị chặn. `[cổng]`
- Khẳng định "nhanh hơn" trong commit phải kèm số trước và sau, preset `profile`, máy đo và số lần
  chạy. `[review]`
- Không tối ưu vi mô làm code khó đọc khi chưa có số đo. Nhưng các hệ nóng (mô phỏng, render, mạng)
  được viết theo hướng dữ liệu từ đầu: mảng liền, duyệt tuần tự, không gọi hàm ảo trong vòng lặp
  nóng. `[review]`

---

## X.9 Bảo mật, ranh giới tin cậy, bản ship

- Server là nguồn sự thật. Client chỉ gửi ý định và input; nó không bao giờ gửi kết quả (sát
  thương, vị trí cuối, vật phẩm nhận được). `[review]`
- Mọi thứ từ mạng đều được kiểm: kích thước, khoảng giá trị, tần suất, và trạng thái (tin nhắn này
  có hợp lệ ở trạng thái hiện tại không). Parser là hàm toàn phần: mọi dãy byte cho ra hoặc một
  tin nhắn hợp lệ, hoặc một lỗi, không bao giờ UB. `[test]`
- Kênh realtime gồm:
  - bắt tay X25519;
  - AEAD cho từng gói, nonce lấy từ số thứ tự;
  - cửa sổ chống replay;
  - connection id tách khỏi cặp IP:port;
  - connect token do auth ký Ed25519, hết hạn ngắn.

  Không gói nào được xử lý trước khi xác thực xong. `[test]`
- Chống khuếch đại: server không trả lời một gói chưa xác thực bằng một gói lớn hơn nó. `[test]`
- Mọi loại tin nhắn và mọi endpoint đều có giới hạn tần suất, theo kết nối và theo tài khoản.
  `[test]`
- Kinh tế: mọi thay đổi về vật phẩm và tiền là một dòng ledger ghi kép trong một transaction của
  PostgreSQL, kèm khoá idempotency. Chỉ `server/lib/ledger` ghi bảng ledger. Crash hay retry ở bất
  kỳ điểm nào cũng không được nhân bản vật phẩm, và có test bơm lỗi để chứng minh điều đó. `[test]`
- Bí mật không bao giờ vào repo. `deploy/local/` chỉ chứa khoá `*.dev.*`, và bản ship từ chối khởi
  động khi thấy khoá dev. `[test]`
- Bản ship (preset `ship`, vùng CHƠI) không có editor, cooker, console debug, lệnh cheat hay Tracy.
  Nó không đọc file rời, chỉ mount pak có trong manifest đã ký, và kiểm hash khi mount. Code dành
  cho dev nằm sau `ORION_DEV_TOOLS` và không được link vào bản ship. `[test]`
- Luau chạy trong sandbox: không có `io` và `os`, không `require` tự do; có ngân sách lệnh cho mỗi
  lần gọi và giới hạn bộ nhớ. `[test]`
- Thêm dependency mới cần ADR ghi license, người duy trì và lịch sử CVE, và được ghim qua baseline
  của vcpkg. `[review]`

---

## X.10 Mạng và protocol

- Tin nhắn chỉ được định nghĩa trong `game/shared/protocol/*.schema`. Code sinh ra nằm trong
  `out/`: không commit, không sửa tay. `[cổng]`
- Đổi schema thì phải tăng `kProtocolVersion`. CI so schema với bản trước và chặn nếu quên tăng.
  `[cổng]`
- Định dạng trên dây là little-endian tường minh và bit-packed. Mọi mảng và chuỗi có giới hạn độ
  dài trong schema. Số thực gửi qua mạng phải khai lượng tử hoá (khoảng và độ chính xác) trong
  schema. Codegen từ chối schema thiếu những khai báo này. `[cổng]`
- Mỗi tin nhắn thuộc đúng một kênh, khai trong schema: unreliable, sequenced, reliable-ordered hoặc
  reliable-unordered. `[cổng]`
- Handler theo thứ tự kiểm rồi mới áp: không đổi trạng thái nào trước khi đã kiểm xong toàn bộ tin
  nhắn. `[review]`
- Server không tin đồng hồ của client. Thời gian là `Tick` của server (ADR 0002). `[review]`
- Các server nói chuyện với nhau cũng qua schema, không qua struct dùng chung. Sự kiện đi qua Redis
  Streams (ví dụ store báo world trao vật phẩm) cũng khai trong schema. `[review]`

---

## X.11 Tất định trong `game/shared` và mô phỏng

- `game/shared` không đọc đồng hồ, không tạo luồng, không dùng RNG toàn cục, không làm I/O. Thời
  gian và RNG được truyền vào. `<chrono>`, `<thread>`, `<random>`, `<fstream>`, `<cmath>` bị cấm
  include trong `game/shared`. `[cổng]`
- RNG là PRNG của `engine/math`, có seed tường minh. `engine/math` cũng cung cấp các hàm toán cần
  thiết (căn, làm tròn, lượng giác tất định) thay cho `<cmath>`. `[cổng]`
- Thứ tự duyệt của `unordered_map`, `unordered_set` hay địa chỉ con trỏ không được ảnh hưởng tới
  trạng thái. `[review]`
- Số thực: không dùng `-ffast-math` hay `/fp:fast` ở bất kỳ đâu. MSVC dùng `/fp:precise`. Tắt hợp
  nhất FMA: clang dùng `-ffp-contract=off`; cờ tương ứng của MSVC còn chờ đo (ARCH §9). `[cổng]`
- Jolt được build với `JPH_CROSS_PLATFORM_DETERMINISTIC` và `JPH_DOUBLE_PRECISION`, ở cả client lẫn
  server. `[cổng]`
- Golden replay ở X.4 phải khớp từng bit giữa MSVC x64, clang Linux x64, NDK arm64, và Apple arm64
  khi đã có runner Mac. Lệch là lỗi chặn. `[test]`

---

## X.12 C++23: phần giao của năm toolchain

- Năm toolchain:
  1. MSVC, với MSVC STL;
  2. clang-cl, với MSVC STL;
  3. clang trên Linux, với libstdc++;
  4. clang của NDK, với libc++;
  5. Apple clang, với libc++.
- Danh sách tính năng đã đo ở hiến pháp III.1 chỉ đúng cho toolchain 1, không áp sang bốn cái còn
  lại. `[review]`
- Code trong T0, T1 và `game/shared` chỉ dùng tính năng có mặt trong
  `tests/toolchain/features.cpp` và xanh trên cả năm. Code server (T4 và các server T5) chỉ cần
  xanh trên toolchain 1 và 3. Code trong một thư mục nền tảng chỉ cần xanh trên toolchain của nền
  tảng đó. `[cổng]`
- Muốn dùng thêm một tính năng: thêm `static_assert` theo feature-test macro vào `features.cpp`,
  chờ CI xanh trên cả năm, rồi mới dùng. Tính năng không có macro thì viết một test biên dịch.
  `[cổng]`
- Những thứ phải đo trước khi dùng, ví dụ: `std::expected` (X.5 dựa vào nó), `std::generator`,
  `std::print`, `std::flat_map`, `std::mdspan`, `std::stacktrace`. C++20 modules và `import std;`
  bị cấm cho tới khi có ADR. `[review]`

---

## X.13 GPU và render

- Chỉ `engine/rhi/<api>/` gọi API đồ hoạ. `engine/render` chỉ thấy interface RHI. `[cổng]`
- Tài nguyên GPU tạm sống trong render graph. Ngoài graph và hàng đợi huỷ trễ, không cấp phát hay
  giải phóng tài nguyên GPU giữa frame. `[review]`
- Không chờ GPU rỗi (`vkDeviceWaitIdle`, chờ fence toàn hàng đợi, `waitUntilCompleted`) trong vòng
  lặp frame. `[review]`
- PSO được tạo lúc load hoặc lấy từ cache, không bao giờ trong frame. Test đếm số PSO tạo trong
  frame. `[test]`
- Mọi đối tượng GPU có tên debug, ở mọi bản trừ `ship`. `[review]`
- Lớp validation bật ở `dev` và `asan`: D3D12 debug layer (thêm GPU-based validation ở `asan`),
  Vulkan validation layers, Metal API validation. Mỗi cảnh báo validation là một bug. `[test]`
- Shader chỉ viết bằng HLSL trong `engine/render/shaders/`; không viết tay SPIR-V hay MSL. Biến thể
  shader được khai báo trước và có giới hạn; không để tổ hợp bùng nổ. `[review]`
- Tier mobile không phải đường phụ: mỗi pass mới khai nó chạy ở tier nào, và tier mobile có ảnh
  vàng riêng. `[test]`

---

## X.14 Dịch vụ HTTP và cơ sở dữ liệu

- Toàn repo chỉ có một ngôn ngữ sản phẩm là C++23 (ADR 0008). Dịch vụ HTTP là server C++ như mọi
  server khác và chịu toàn bộ luật từ X.1 đến X.13. Không thêm ngôn ngữ thứ hai khi chưa có ADR.
  `[review]`
- Dịch vụ HTTP chỉ nói HTTP/1.1 sau load balancer; TLS kết thúc ở load balancer. Không mở cổng HTTP
  của service thẳng ra internet. `[review]`
- Mỗi request có giới hạn cứng về kích thước header, kích thước body và thời gian đọc. Vượt giới
  hạn thì trả lỗi 4xx và đóng kết nối. `[test]`
- Body JSON được đọc bằng simdjson qua API mã lỗi. Mỗi endpoint có một parser request có kiểu, và
  mỗi parser có một target fuzz (X.4). `[test]`
- Mật khẩu băm bằng Argon2id qua `engine/crypto`. So sánh bí mật (token, hash, chữ ký) bằng hàm so
  sánh thời gian hằng, không bằng `==`. Không tự chế thuật toán mã hoá. `[review]`
- Connect token chỉ được tạo và kiểm trong `engine/net`. Auth và gateway gọi cùng một code.
  `[review]`
- Mọi truy vấn SQL dùng tham số qua `server/lib/db` (`PQexecParams` hoặc prepared statement),
  không nối chuỗi. `[review]`
- Mọi lời gọi ra ngoài (DB, Redis, HTTP) có timeout, và mỗi request có một deadline. `[review]`
- Dịch vụ HTTP không chạm mô phỏng hay protocol realtime. Cần báo cho world thì phát sự kiện qua
  Redis Streams, định dạng khai trong schema (X.10). `[review]`
- Migration là file SQL đánh số trong `db/migrations/`, áp bằng `orion_migrate`, mỗi file một
  transaction. Chỉ được thêm mới; không sửa migration đã merge. Mỗi migration có test áp lên DB
  rỗng và lên schema của bản trước. `[test]`

---

## X.15 Tài liệu, comment, commit

- Comment nói vì sao, không nói làm gì. Không để lại code bị comment. `[review]`
- Mỗi hàm và kiểu public ghi hợp đồng của nó: tiền điều kiện, ai sở hữu gì, gọi từ luồng nào, và
  độ phức tạp nếu không hiển nhiên. `[review]`
- `TODO` phải có mã, ví dụ `TODO(NGHI-NGO-012)`. Không có TODO trần. `[review]`
- Đổi hợp đồng (định dạng cooked, pak, protocol, tầng) thì sửa `docs/formats/` hoặc ARCH trong
  cùng commit. `[review]`
- Commit nhỏ, mỗi commit một thay đổi logic, và mọi commit đều build và test xanh. Không trộn
  refactor với tính năng. `[review]`
- Tiêu đề commit có dạng `<module>: <việc đã làm>`, ví dụ `engine/net: thêm cửa sổ chống replay`.
  `[review]`

---

## X.16 Cách Claude làm việc trong repo này

**Trước khi viết:**

1. Đọc code hiện có của module, test của nó, và mục liên quan trong ARCH.
2. Việc chạm trụ cột (ARCH §5), thêm dependency, thêm thư mục cấp cao, hay đổi protocol: dừng lại,
   nêu phương án, chờ người quyết.
3. Không chắc một API hay một hành vi thì tra nguồn chính thức hoặc đo. Không kiểm được thì mở một
   mục trong sổ nghi ngờ. Không đoán.

**Trong khi viết:**

4. Viết diff nhỏ nhất đạt mục tiêu. Không sửa ngoài phạm vi được giao; thấy lỗi khác thì báo lại,
   không lặng lẽ sửa.
5. Không tắt warning, test, sanitizer hay check để build xanh. Không sửa `.clang-format`, code sinh
   ra, hay migration đã merge.

**Chỉ nói "xong" khi mọi mục áp dụng dưới đây đã chạy và xanh:**

6. Cổng kiểm xanh.
7. Build không warning ở preset `dev` và `asan`; thêm `linux` nếu chạm T0, T1, `game/shared` hay
   server. Lệnh build theo ARCH §1 và §6.
8. `ctest` xanh, và có test mới cho hành vi mới hoặc bug vừa sửa.
9. Chạm parser: fuzz target của nó chạy ít nhất 10 phút không crash.
10. Chạm `game/shared` hay mô phỏng: golden replay khớp.
11. Chạm hot path: benchmark trước và sau, kèm số.
12. ARCH, ADR, `docs/formats/` và sổ nghi ngờ đã được cập nhật nếu cần.

**Báo cáo:**

13. Liệt kê lệnh đã chạy và kết quả thật. Bước nào chưa chạy được thì nói rõ bước đó chưa chạy và
    vì sao. Không bao giờ viết "chắc là chạy được".
