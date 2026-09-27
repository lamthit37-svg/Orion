# Orion — kiến trúc, bố cục, công nghệ

File này chốt ba thứ: **cái gì nằm ở đâu**, **mỗi phần dùng công nghệ gì**, và **ba cách dùng
repo — DEV, CHẠY, CHƠI**. Luật viết code nằm ở `CLAUDE.md`, không lặp lại ở đây.

Đổi một mục ở §5 là đổi trụ cột: viết ADR trong `docs/adr/` trước, sửa file này sau, rồi mới sửa
code.

**Trạng thái: đề xuất, chưa dựng, chưa build.** Tên preset, target, lệnh và port vcpkg dưới đây
là hợp đồng dự kiến. Những gì chưa đo liệt kê ở §9 — kiểm hết trước khi coi file này là sự thật.

Trên máy dev Windows, mọi lệnh `cmake` và `ctest` trong file này chạy bên trong cổng `dev.bat`
(hiến pháp II). Script trong repo viết bằng Python 3.13, theo tiền lệ cổng kiểm của Orion (hiến
pháp V.5), vì chúng phải chạy cả trên runner Linux và Mac. CMake gọi script qua
`Python3_EXECUTABLE`; gọi tay thì dùng `py` (hiến pháp VIII).

---

## 1. Ba cách dùng repo

Mọi thứ sinh ra nằm trong `out/`, bị gitignore. Xoá `out/` lúc nào cũng được mà không mất gì. Xoá
đi mà mất thứ gì thì thứ đó đang nằm sai chỗ.

| | DEV | CHẠY | CHƠI |
|---|---|---|---|
| Để làm gì | viết, build, lặp nhanh | dựng cả thế giới trên một máy | chơi đúng bản người chơi nhận |
| Thư mục | `out/build/<preset>/` | `out/run/` | `out/play/<platform>/` |
| Dựng | `cmake --workflow --preset dev` | `cmake --build --preset dev --target stage_run` | `cmake --workflow --preset ship-win64` |
| Mở | `out/build/dev/bin/orion_client.exe` | `py tools/run/cluster.py up` | `out/play/win64/Orion.exe` |
| Dữ liệu | cooked dạng file rời, hot reload | cooked file rời, PostgreSQL, Redis | pak nén, manifest đã ký |
| Công cụ dev | console, Tracy, editor, lệnh cheat | log, metrics, bot | không có gì |

### 1.1 DEV — viết và lặp

- Dựng và chạy test: `cmake --workflow --preset dev`.
- Cook tăng dần: target `cook_win64` ghi vào `out/cooked/win64/`. Chỉ nguồn đã đổi mới bị cook
  lại; cache theo hash nội dung.
- Client: `orion_client --data out/cooked/win64 --realm local`. Hot reload shader và `data/`, có
  console debug và Tracy.
- Editor: `orion_editor` — sửa `content/maps/`, lưu thẳng về nguồn.
- Việc khác thì dùng preset khác: `asan`, `ubsan`, `profile`, `linux` (§6).

### 1.2 CHẠY — cả thế giới trên một máy

```
out/run/
├── bin/ — mọi tiến trình server
│   ├── realtime: orion_gateway, orion_world, orion_instance
│   ├── nền: orion_social, orion_market, orion_persistence
│   └── HTTP: orion_auth, orion_account, orion_store, orion_patch, orion_admin
├── config/ — sinh từ deploy/local/: port, địa chỉ DB, khoá dev
├── logs/ — một file JSON lines cho mỗi tiến trình
└── cluster.json — PID và port của các tiến trình đang chạy
```

- Lệnh: `py tools/run/cluster.py up`, `down`, `status`, `logs <tiến-trình>`, `bots <số-lượng>`.
- `up` bật theo thứ tự: kiểm PostgreSQL và Redis → áp migration bằng `orion_migrate` →
  persistence → social, market → world, instance → gateway → dịch vụ HTTP. `down` tắt theo thứ tự
  ngược lại.
- Máy dev không có Docker (hiến pháp VIII), nên PostgreSQL 18 và Redis 8 cài thẳng trong WSL2.
  `deploy/docker/compose.yaml` chỉ dành cho CI và staging.
- Server trong `out/run/` là bản Windows (MSVC), để debug cùng một toolchain với client. Bản
  production là Linux (preset `linux`). Cả hai phải build sạch.
- Chơi với cụm local: client dev hoặc bản CHƠI, thêm `--realm local`.

### 1.3 CHƠI — đúng bản người chơi nhận

```
out/play/
├── win64/
│   ├── Orion.exe
│   ├── D3D12/ — D3D12Core.dll của Agility SDK, đi kèm game
│   ├── data/ — *.pak nén Zstd, đặt tên theo hash nội dung
│   └── manifest.bin — danh sách pak và hash, có chữ ký Ed25519
├── android/ — orion.aab, Gradle gọi CMake preset android-arm64
└── ios/ — chỉ dựng được trên Mac
```

- Bản CHƠI không có editor, cooker, console debug hay Tracy, và không đọc file rời (CLAUDE.md X.9).
- Android: chạy Gradle `bundleRelease` trong `game/client/android/`, kết quả chép vào
  `out/play/android/`.
- iOS: trên Mac, `cmake --preset ios-arm64` sinh dự án Xcode, archive ra `out/play/ios/`.

---

## 2. Cây thư mục

```
EngineGAME/
├── CLAUDE.md — luật dự án, mục X của hiến pháp máy
├── README.md — dựng, chạy, chơi trong năm phút
├── CMakeLists.txt — chỉ add_subdirectory, không chứa logic
├── CMakePresets.json — preset và workflow ở §6
├── vcpkg.json — dependency, ghim builtin-baseline
├── vcpkg-configuration.json
├── .clang-format — bất biến (hiến pháp V.4)
├── .clang-tidy
├── .editorconfig
├── .gitattributes — Git LFS cho content/, quy ước xuống dòng
├── .gitignore — out/, vcpkg_installed/
│
├── cmake/
│   ├── orion_flags.cmake — orion_apply_flags(): cờ nền cho mọi toolchain
│   ├── orion_module.cmake — orion_add_module(): target, test, tầng
│   ├── orion_stage.cmake — target cook_<platform>, stage_run, stage_play
│   ├── toolchains/ — linux-clang, android, ios
│   └── triplets/ — triplet vcpkg riêng của dự án
│
├── engine/ — engine chung, không biết gì về MMO
│   ├── core/ — cấp phát, log, assert, handle, chuỗi, thời gian, platform.hpp
│   ├── math/ — vector, quaternion, WorldPos, lượng giác tất định, PRNG
│   ├── jobs/ — job system, cầu nối JobSystem cho Jolt
│   ├── crypto/ — bọc libsodium: AEAD, X25519, Ed25519, BLAKE2b, Argon2id
│   ├── io/ — VFS, pak, đọc bất đồng bộ
│   ├── net/ — socket, transport UDP, reliability, bitstream, mã hoá kênh
│   ├── asset/ — đọc định dạng cooked, handle, streaming
│   ├── physics/ — bọc Jolt
│   ├── anim/ — bọc ozz-animation
│   ├── nav/ — bọc Detour
│   ├── script/ — bọc Luau
│   ├── platform/ — cửa sổ, input, vòng đời app qua SDL3; không vào server
│   ├── rhi/ — interface chung, d3d12/, vulkan/, metal/; không vào server
│   ├── render/ — render graph, renderer, shaders/; không vào server
│   ├── audio/ — bọc FMOD; không vào server
│   └── ui/ — RmlUi, FreeType, HarfBuzz; không vào server
│
├── game/
│   ├── shared/ — luật chơi thuần, dùng chung cho client và server
│   │   ├── protocol/ — *.schema, nguồn cho codegen
│   │   ├── movement/
│   │   ├── combat/
│   │   └── defs/ — kiểu dữ liệu item, skill, quest
│   ├── client/ — logic game phía client, chia theo module
│   │   ├── win/ — điểm vào Windows
│   │   ├── android/ — dự án Gradle, gọi CMake
│   │   └── apple/ — vỏ iOS, dự án Xcode sinh bằng CMake
│   ├── server/
│   │   ├── lib/ — thư viện dùng chung của các server (T4)
│   │   │   ├── db/ — bọc libpq
│   │   │   ├── http/ — bọc Boost.Beast
│   │   │   └── ledger/ — ghi kép, khoá idempotency; nơi duy nhất ghi bảng ledger
│   │   ├── gateway/ — nhận kết nối, kiểm token, định tuyến vào zone
│   │   ├── world/ — zone: mô phỏng 50 Hz, AOI, NPC, combat
│   │   ├── instance/ — dungeon, arena, boss
│   │   ├── social/ — chat, party, guild, mail
│   │   ├── market/ — đấu giá, giao dịch
│   │   ├── persistence/ — ghi nhân vật và vật phẩm vào PostgreSQL
│   │   ├── auth/ — HTTP: đăng nhập, cấp connect token
│   │   ├── account/ — HTTP: hồ sơ, danh sách nhân vật
│   │   ├── store/ — HTTP: mua hàng
│   │   ├── patch/ — HTTP: phát manifest đã ký
│   │   └── admin/ — HTTP: công cụ vận hành, GM
│   └── bot/ — client không đồ hoạ, dùng để thử tải
│
├── data/ — dữ liệu thiết kế: *.json và schema/
├── content/ — asset nguồn qua Git LFS: models/, textures/, audio/, maps/, ui/
├── db/
│   └── migrations/ — SQL đánh số, áp bằng orion_migrate
│
├── tools/
│   ├── check_style.py — cổng hiện có (hiến pháp V.5), giữ nguyên đường dẫn
│   ├── check_layers.py — luật include ở CLAUDE.md X.1
│   ├── check_tracked.py — tệp nguồn bị .gitignore nuốt
│   ├── gatelib.py — phần dùng chung của ba cổng trên
│   ├── tests/ — unittest của các cổng kiểm, chạy trong ctest
│   ├── cooker/ — orion_cooker
│   ├── editor/ — orion_editor: world editor trên Dear ImGui
│   ├── codegen/ — sinh C++ từ *.schema
│   ├── migrate/ — orion_migrate: áp db/migrations/ theo thứ tự, mỗi file một transaction
│   ├── shaderc/ — HLSL ra DXIL, SPIR-V, Metal
│   └── run/ — cluster.py: bật, tắt, xem log, thả bot
│
├── tests/ — kiểm thử liên module
│   ├── toolchain/ — features.cpp: static_assert từng feature-test macro
│   ├── replay/ — golden replay cho game/shared
│   ├── fuzz/ — target libFuzzer và corpus/
│   └── soak/ — kịch bản bot swarm
│
├── deploy/
│   ├── local/ — cấu hình cụm máy dev, khoá giả *.dev.*
│   ├── docker/ — Dockerfile server Linux, compose cho CI và staging
│   └── observability/ — OTel Collector, Prometheus, dashboard Grafana
│
├── third_party/ — chỉ thứ vcpkg không có, ghim commit (ví dụ metal-cpp)
├── docs/
│   ├── ARCHITECTURE.md — file này
│   ├── NGHI-NGO.md — sổ nghi ngờ (CLAUDE.md X.6)
│   ├── adr/ — quyết định trụ cột, đánh số
│   └── formats/ — định dạng cooked, pak, protocol
│
└── out/ — gitignore, xoá lúc nào cũng được
    ├── build/<preset>/ — DEV
    ├── cooked/<platform>/ — cook dạng file rời, dùng chung cho DEV và CHẠY
    ├── run/ — CHẠY
    └── play/<platform>/ — CHƠI
```

Quy ước module (luật đầy đủ ở CLAUDE.md X.2): một module là một thư mục và một target; `.hpp` nằm
cạnh `.cpp` (hiến pháp IV); test của module nằm ở `<module>/tests/`; code riêng từng nền tảng nằm
trong thư mục con `win/`, `linux/`, `android/`, `apple/` của module.

---

## 3. Tầng phụ thuộc

Tầng thấp không bao giờ biết tầng cao. `tools/check_layers.py` thi hành luật này, và CMake chỉ link
theo đúng chiều.

- **T0 nền** — `engine/core`, `engine/math`. Chỉ phụ thuộc thư viện chuẩn và API hệ điều hành.
- **T1 hệ thống** — `engine/jobs`, `crypto`, `io`, `net`, `asset`, `physics`, `anim`, `nav`,
  `script`. Phụ thuộc T0.
- **T2 đồ hoạ và thiết bị** — `engine/platform`, `rhi`, `render`, `audio`, `ui`. Phụ thuộc T0, T1.
  Không bao giờ vào server.
- **T3 luật chơi** — `game/shared`. Phụ thuộc T0, T1; không bao giờ T2.
- **T4 thư viện server** — `game/server/lib/`: `db`, `http`, `ledger`. Phụ thuộc T0, T1, T3. Chỉ
  server T5 và `tools/` được dùng.
- **T5 sản phẩm** — `game/client` dùng T0 đến T3. Các server trong `game/server/` dùng T0, T1, T3,
  T4. `game/bot` dùng T0, T1, T3. `tools/*` dùng mọi tầng engine, T3 và T4.
- Không ai phụ thuộc `tools/`, `game/bot`, hay một sản phẩm T5 khác.
- Các server T5 không include chéo nhau. Chúng nói chuyện qua mạng bằng `game/shared/protocol`, hoặc
  qua PostgreSQL và Redis Streams với định dạng khai trong schema (CLAUDE.md X.10).

---

## 4. Công nghệ cho từng phần

Mọi thư viện ngoài vào qua vcpkg manifest; port nào vcpkg không có thì vào `third_party/` và ghim
commit. Tên port chưa kiểm (§9).

### 4.1 Nền dùng chung — client, server, tool

| Phần | Chọn | Ghi chú |
|---|---|---|
| Ngôn ngữ | C++23; C11 cho C thuần | chỉ phần giao của năm toolchain (CLAUDE.md X.12) |
| Build | CMake 4.4, Ninja, vcpkg manifest | preset ở §6 |
| Toán | tự viết trong `engine/math`; SIMD SSE4.2 trên x64, NEON trên ARM64 | cần kiểm soát determinism và `WorldPos` double |
| Job | tự viết, work-stealing | cấp JobSystem cho Jolt, chạy pha song song của server |
| Log | tự viết trên `std::format`, ghi bất đồng bộ | server ghi JSON lines |
| Crypto, hash | libsodium, bọc trong `engine/crypto` | X25519, AEAD, Ed25519, BLAKE2b, Argon2id — một thư viện cho tất cả |
| Nén | Zstd | pak, patch; dictionary cho file nhỏ |
| Profiling | Tracy | client, server, tool; bị loại ở `ship` |
| Crash | sentry-native (Crashpad) | minidump từ client và server |
| Test | GoogleTest, ctest, Google Benchmark, libFuzzer | CLAUDE.md X.4 |

### 4.2 Client theo nền tảng

| Phần | Windows | Android | iOS |
|---|---|---|---|
| Graphics API | D3D12 + Agility SDK, D3D12MA | Vulkan, volk, VMA | Metal 4 qua metal-cpp (ADR 0005) |
| Shader | HLSL → DXC → DXIL | HLSL → DXC → SPIR-V | DXIL → Metal Shader Converter |
| Cửa sổ, input, vòng đời | SDL3 | SDL3 | SDL3 |
| Đọc asset | DirectStorage | AAssetManager, file thường | file thường |
| Soi GPU | PIX | RenderDoc, Android GPU Inspector | Xcode Metal debugger |
| Đóng gói | thư mục và launcher | AAB, Play Asset Delivery | Xcode archive, chỉ trên Mac |

SDL3 chỉ lo cửa sổ, input và vòng đời app. Render đi thẳng xuống API gốc, không qua SDL.

### 4.3 Engine phía client

| Phần | Chọn | Ghi chú |
|---|---|---|
| Kiến trúc render | RHI riêng, render graph, bindless | binding model ở ADR 0003 |
| Tier cao (PC) | GPU-driven: cull bằng compute, Hi-Z, indirect draw; mesh shader là tuỳ chọn | cull bằng compute chạy được mọi nơi |
| Tier mobile | forward, cull đơn giản, ít pass | GPU tile-based chịu phạt nặng vì pass thừa |
| Animation | ozz-animation, GPU skinning | một skeleton cho mỗi họ nhân vật |
| Physics | Jolt, double precision | chỉ để dự đoán; server quyết |
| UI game | RmlUi, FreeType, HarfBuzz | tiếng Việt có dấu, CJK, IME |
| UI dev, editor | Dear ImGui, nhánh docking | không vào bản ship |
| Audio | FMOD Studio | kiểm license (§9) |
| Script | Luau | UI, cutscene; có sandbox |

### 4.4 Asset pipeline — `tools/cooker`

Importer đọc thẳng vào dạng trung gian riêng của cooker. glTF là đầu vào ưu tiên, không phải trạm
chuyển bắt buộc, vì đổi FBX sang glTF làm rơi dữ liệu.

| Việc | Chọn |
|---|---|
| Đọc FBX | ufbx |
| Đọc glTF, GLB | fastgltf |
| Mesh: tối ưu, LOD, meshlet | meshoptimizer |
| Container texture | KTX2 qua libktx |
| Nén BC7, BC5 cho PC | bc7enc_rdo |
| Nén ASTC cho mobile | astc-encoder |
| Bake navmesh | Recast, dạng tiled |
| Bake collision | Jolt, shape đã serialize |
| Animation | công cụ offline của ozz-animation |
| Audio | bank của FMOD Studio |
| Pak và patch | Zstd, manifest theo hash BLAKE2b, ký Ed25519 |
| Data thiết kế | JSON kiểm bằng JSON Schema (nlohmann-json, json-schema-validator), biên dịch ra bảng nhị phân |

Texture được cook thẳng ra định dạng gốc của từng nền tảng. BasisU không dùng, vì mỗi nền tảng đã
có gói riêng.

### 4.5 Server C++ — `game/server`

| Phần | Chọn | Ghi chú |
|---|---|---|
| OS | Linux x86-64, bản LTS mới nhất lúc triển khai | |
| Toolchain | clang, libstdc++ | bản Windows MSVC chỉ để debug trên máy dev |
| Mạng | UDP, epoll, recvmmsg và sendmmsg | io_uring khi đo thấy lợi |
| Bảo mật kênh | `engine/net` trên `engine/crypto`: X25519, AEAD, connect token Ed25519 | CLAUDE.md X.9 |
| Mô phỏng | EnTT, chỉ trong world và instance | phần còn lại của engine không dùng ECS |
| Physics | Jolt double precision: character kinematic, truy vấn thế giới tĩnh | không chạy rigid-body đầy đủ cho người chơi |
| Nav | Detour, DetourTileCache | |
| Script NPC, quest | Luau: sandbox, ngân sách lệnh mỗi tick | |
| DB | libpq, bọc trong `server/lib/db` | pipeline mode ở `persistence` |
| Cache, sự kiện | hiredis, Redis Streams | |
| Quan sát | opentelemetry-cpp → OTel Collector | |

### 4.6 Dịch vụ HTTP — `game/server` (C++)

Năm dịch vụ không realtime, viết bằng C++23 như mọi server khác (ADR 0008).

| Dịch vụ | Việc |
|---|---|
| auth | đăng nhập, băm mật khẩu Argon2id, cấp connect token ký Ed25519; gateway kiểm chữ ký tại chỗ, không cần RPC |
| account | hồ sơ, danh sách nhân vật |
| store | mua hàng: ghi qua `server/lib/ledger`, phát sự kiện qua Redis Streams để world trao vật phẩm |
| patch | phát manifest đã ký, trỏ tới CDN |
| admin | công cụ vận hành, GM |

| Phần | Chọn | Ghi chú |
|---|---|---|
| HTTP | Boost.Beast trên Boost.Asio, bọc trong `server/lib/http` | chỉ HTTP/1.1, đứng sau load balancer |
| TLS 1.3, HTTP/3 | kết thúc ở load balancer hoặc CDN | service không giữ chứng chỉ |
| JSON | simdjson để đọc, qua API mã lỗi; writer nhỏ tự viết để ghi | body từ ngoài, phải fuzz |
| DB | libpq qua `server/lib/db` | truy vấn có tham số |
| Cache, rate limit | hiredis | |
| Mật khẩu, token | `engine/crypto`, `engine/net` | một bản code, dùng chung với gateway |
| Ledger | `server/lib/ledger` | một bản code, dùng chung với persistence |
| Quan sát | opentelemetry-cpp | |

Vì sao C++ chứ không phải ngôn ngữ thứ hai: code ký token và ghi ledger — hai chỗ nhạy cảm nhất —
chỉ có một bản, dùng chung với gateway và persistence. Cả repo có một toolchain, một bộ luật, một
CI. Cái giá là code CRUD dài hơn, và phần mở ra internet phải được fuzz kỹ (CLAUDE.md X.4, X.14).

### 4.7 Dữ liệu và hạ tầng

| Phần | Chọn |
|---|---|
| DB chính | PostgreSQL, major ổn định mới nhất lúc triển khai (hiện là 18) |
| Cache, presence, rate limit, sự kiện | Redis 8 |
| Asset và patch | object storage S3-compatible, CDN |
| Container | image OCI cho server và services |
| Điều phối | compose cho staging; Kubernetes và Agones khi scale thật sự cần |
| Quan sát | OpenTelemetry, Prometheus, Grafana |

---

## 5. Quyết định nền — viết thành ADR trước dòng code đầu tiên

Mỗi mục là một file `docs/adr/NNNN-ten.md`. Đây là những thứ mà làm sai thì phải đập lại cả hệ.

- **0001 Hệ toạ độ** — tay phải, trục Y hướng lên, đơn vị mét, trùng glTF; cooker chuyển mọi nguồn
  về hệ này. `WorldPos` dùng double trong mô phỏng và `game/shared`. Jolt build double precision ở
  cả client lẫn server. Render dùng toạ độ float tương đối camera.
- **0002 Thời gian** — server tick 50 Hz, đếm bằng `Tick` 64-bit. Tần suất gửi snapshot tách khỏi
  tick, thích ứng theo ưu tiên và băng thông. Client nội suy với độ trễ cố định tính bằng tick.
- **0003 Binding RHI** — bindless: descriptor heap trên D3D12, descriptor indexing trên Vulkan,
  argument buffer trên Metal, khớp mô hình top-level argument buffer của Metal Shader Converter.
  Nếu sàn Android buộc phải có đường không bindless thì ghi rõ ở đây.
- **0004 Protocol** — schema cộng codegen, bit-packed, `kProtocolVersion`. Không tương thích ngược
  giữa các bản; client cũ bị buộc cập nhật.
- **0005 Sàn thiết bị** — PC có tier thấp và tier cao; DX12 Ultimate là tier cao, không phải sàn,
  và sàn chốt bằng khảo sát phần cứng thật. Sàn Vulkan trên Android chốt bằng số liệu thiết bị
  thật. iOS: chọn chỉ Metal 4 (iOS 26 trở lên, iPhone 12 trở lên) hay giữ thêm Metal 3. Máy tham
  chiếu cho ngân sách ở CLAUDE.md X.8 cũng chốt ở đây.
- **0006 Định danh** — `EntityId` 64-bit có generation, chỉ sống trong một tiến trình. Id bền
  (nhân vật, vật phẩm) là 64-bit duy nhất toàn cục, do persistence cấp.
- **0007 Mô hình luồng** — §7.
- **0008 Một ngôn ngữ** — mọi code chạy trong sản phẩm, kể cả dịch vụ HTTP (§4.6), là C++23; C
  thuần dùng C11. Ngoài sản phẩm: script trong repo là Python (hiến pháp V.5), shader là HLSL,
  script gameplay là Luau. Thêm một ngôn ngữ thứ hai cần một ADR mới thay ADR này.

---

## 6. Preset, toolchain, target

| Preset | Toolchain | Dùng cho |
|---|---|---|
| `dev` | MSVC, Debug | mặc định, lặp hằng ngày |
| `asan` | MSVC, Debug, `/fsanitize=address` | bộ nhớ, con trỏ, parser |
| `ubsan` | clang-cl, `-fsanitize=undefined` | UB số học, bit packing |
| `profile` | MSVC, RelWithDebInfo, bật Tracy | đo hiệu năng |
| `ship` | MSVC, Release, LTCG; loại Tracy và công cụ dev | bản CHƠI trên Windows |
| `linux` | clang, trong WSL2 hoặc CI | server, shared, tool, fuzz |
| `linux-tsan` | clang, `-fsanitize=thread` | race ở server, jobs, net |
| `android-arm64` | clang của NDK, gọi từ Gradle | client Android |
| `ios-arm64` | Apple clang, generator Xcode, chỉ trên Mac | client iOS |

Workflow preset:

- `dev` — configure, build, test với preset `dev`.
- `ship-win64` — configure `ship`, rồi build target `cook_win64` và `stage_play`.

Tên target (hiến pháp IV.1):

- Thư viện: `engine_<module>`, `game_shared`, `game_client`, `server_<tên>`, `tool_<tên>`.
- Chương trình: `orion_client`, `orion_gateway`, `orion_world`, `orion_instance`, `orion_social`,
  `orion_market`, `orion_persistence`, `orion_auth`, `orion_account`, `orion_store`, `orion_patch`,
  `orion_admin`, `orion_bot`, `orion_cooker`, `orion_editor`, `orion_migrate`.
- Target tuỳ biến: `orion_check_style` (cổng kiểm), `cook_<platform>`, `stage_run`, `stage_play`.
- `engine_core` phụ thuộc `orion_check_style`, nên mọi target đều phụ thuộc cổng một cách gián
  tiếp (hiến pháp V.5).

---

## 7. Mô hình luồng

- **world, instance** — mỗi zone một luồng mô phỏng; một tiến trình có thể chạy nhiều zone. Mỗi
  tick có các pha: nhận input → movement, combat → AI (tần suất thích ứng) → truy vấn physics → AOI
  → replication. Pha song song chạy trên job worker và gộp kết quả ở cuối pha. Một luồng IO mạng
  trao gói với luồng mô phỏng qua hàng đợi. Lệnh ghi được đẩy bất đồng bộ sang `persistence`.
- **gateway** — chỉ có luồng IO, không giữ trạng thái game.
- **persistence** — một luồng nhận lệnh ghi, gom thành lô, gửi qua libpq pipeline.
- **dịch vụ HTTP** — một nhóm luồng IO cố định của Boost.Asio. Truy vấn DB không được chặn luồng
  IO: dùng API bất đồng bộ của libpq hoặc một pool kết nối riêng.
- **client** — luồng chính lo sự kiện SDL, logic game và dựng dữ liệu frame. Luồng render chạy
  render graph và ghi command list song song qua job. Thêm một luồng IO streaming, một luồng audio
  do FMOD tự quản, và job worker bằng số core trừ 2.

---

## 8. CI tối thiểu

1. Cổng kiểm — chạy đầu tiên vì nhanh nhất.
2. Windows: build và test preset `dev`; build và test preset `asan`; test ảnh vàng trên WARP.
3. Linux: build server, shared, tool; test; coverage bằng llvm-cov; golden replay; test migration
   trên PostgreSQL thật; TSan khi đã đo được (§9).
4. clang-tidy trên compile database của preset `dev` và `linux`.
5. Benchmark trên một runner cố định, so với baseline; chậm hơn 5% thì chặn (CLAUDE.md X.8).
6. Fuzz: chạy mỗi đêm, mỗi target một khoảng cố định, corpus được tích luỹ.
7. Android: build preset `android-arm64`.
8. macOS: build iOS, khi đã có runner Mac.
9. Ship: mỗi đêm dựng `out/play/` cho mọi nền tảng, rồi chạy smoke test bằng bot vào cụm staging.

---

## 9. Chưa đo — phải kiểm khi dựng

Mỗi dòng dưới đây có một mục trong `docs/NGHI-NGO.md` cho tới khi đo xong.

- Port vcpkg cho từng thư viện ở §4: tên, phiên bản, triplet. Thiếu port thì vào `third_party/`
  kèm ADR.
- Mọi thư viện ngoài build được khi tắt exception và RTTI (CLAUDE.md X.3). Rủi ro nhất là
  Boost.Beast và Boost.Asio: dùng được với `BOOST_NO_EXCEPTIONS`, hay phải bọc `server/lib/http`
  thành module bật exception và bắt hết ở ranh giới.
- simdjson: API mã lỗi dùng được đầy đủ khi tắt exception.
- clang và TSan trong WSL2. Hiến pháp II.2 chỉ nói TSan không có trên clang-cl cho Windows; đường
  Linux chưa đo trên máy này.
- CMake có phát `/external:I` cho include SYSTEM với MSVC để chặn warning từ header ngoài hay không.
- Cờ tắt hợp nhất FMA tương ứng trên MSVC, để golden replay khớp giữa các toolchain (CLAUDE.md
  X.11).
- Android SDK và NDK chưa có trên máy. JDK 25 có sẵn, nhưng cần kiểm Android Gradle Plugin có nhận
  JDK 25 không.
- Chưa có Mac cho iOS.
- Giao tính năng C++23 của năm toolchain: điền `tests/toolchain/features.cpp` bằng số đo, không
  bằng trí nhớ.
- Điều khoản license hiện hành của FMOD.
- Ngân sách ở CLAUDE.md X.8: đều là giả định cho tới lần đo đầu tiên.
