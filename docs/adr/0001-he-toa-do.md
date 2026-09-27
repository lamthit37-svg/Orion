# ADR 0001 — Hệ toạ độ

- **Trạng thái:** Chấp nhận
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.3, §4.4, §5; CLAUDE.md X.11

## Bối cảnh

Client, server, cooker và physics phải cùng hiểu một vị trí nghĩa là gì. Sai hệ toạ độ hay đơn vị
thì mọi asset, mọi bản ghi replay và mọi gói tin đều phải làm lại. Thế giới MMO rộng hàng chục km:
float 32-bit ở toạ độ 20 000 m chỉ còn độ phân giải khoảng 2 mm, không đủ cho mô phỏng tất định và
physics.

## Quyết định

1. **Hệ trục:** tay phải, `+Y` hướng lên, trùng glTF 2.0. Mặt trước của một asset nhìn về `+Z`, nên
   bên phải của asset là `-X`.
2. **Đơn vị:** 1.0 là một mét. Góc trong code và trên dây tính bằng radian.
3. **Quay:** quaternion đơn vị `(x, y, z, w)`. Yaw là góc quay quanh `+Y`; yaw 0 nhìn về `+Z`;
   yaw dương quay ngược chiều kim đồng hồ khi nhìn từ trên xuống, nên yaw `+π/2` đưa `+Z` về `+X`.
4. **`WorldPos`:** ba số `f64`, định nghĩa trong `engine/math`. Mô phỏng, `game/shared` và mọi
   server lưu vị trí thế giới bằng `WorldPos`. Độ lệch cục bộ (offset, vận tốc, pháp tuyến) dùng
   vector `f32`.
5. **Render:** GPU không bao giờ thấy toạ độ thế giới tuyệt đối. CPU tính `WorldPos - camera` bằng
   `f64` rồi mới đổi sang `f32`; mọi dữ liệu frame là toạ độ tương đối camera.
6. **Physics:** Jolt build với `JPH_DOUBLE_PRECISION` và `JPH_CROSS_PLATFORM_DETERMINISTIC` ở cả
   client lẫn server (CLAUDE.md X.11).
7. **Cooker:** mọi nguồn được đổi về hệ này lúc cook. glTF đã khớp sẵn; FBX và nguồn khác được
   đổi trục và đơn vị trong importer, không để runtime đổi.
8. **Trên dây:** vị trí gửi qua mạng được lượng tử hoá theo khoảng và độ chính xác khai trong
   schema (ADR 0004, CLAUDE.md X.10), không gửi `f64` trần.

## Hệ quả

- `engine/math` phải có `WorldPos` và phép trừ ra vector `f32` tương đối, kèm test biên độ lớn.
- Asset Z-up (Blender, 3ds Max) được đổi trong cooker; test của cooker có ca đổi trục.
- Camera-relative rendering là bắt buộc, không phải tối ưu.
- Replay và snapshot lưu `f64`; so khớp từng bit giữa toolchain dựa vào CLAUDE.md X.11.

## Phương án đã loại

- Z-up (Unreal, Blender): lệch glTF, mọi asset glTF phải đổi trục.
- Tay trái (D3D, Unity): lệch glTF và lệch quy ước toán học của Jolt.
- Toạ độ `f32` kèm gốc dịch chuyển (origin rebasing): server phải rebasing theo từng người chơi,
  phức tạp và dễ lệch tất định hơn `f64`.
