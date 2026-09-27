# ADR 0008 — Một ngôn ngữ

- **Trạng thái:** Chấp nhận
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.6, §5; CLAUDE.md X.3, X.12, X.14

## Bối cảnh

Dịch vụ HTTP thường được viết bằng một ngôn ngữ thứ hai. Nhưng hai chỗ nhạy cảm nhất của hệ, ký
connect token và ghi ledger, nằm đúng giữa dịch vụ HTTP và server realtime. Hai bản code cho hai
chỗ này là hai nơi để lệch nhau.

## Quyết định

1. Mọi code chạy trong sản phẩm, kể cả năm dịch vụ HTTP, là C++23 trong phần giao của năm toolchain
   (CLAUDE.md X.12). C thuần dùng C11.
2. Ngoài sản phẩm: script trong repo là Python (hiến pháp V.5); shader là HLSL; script gameplay là
   Luau.
3. Thêm một ngôn ngữ thứ hai cần một ADR mới thay ADR này.

## Hệ quả

- Ký và kiểm connect token chỉ có một bản trong `engine/net`; ghi ledger chỉ có một bản trong
  `server/lib/ledger`.
- Cả repo có một toolchain, một bộ luật, một CI.
- Code CRUD dài hơn; phần mở ra internet phải được fuzz kỹ (X.4, X.14).

## Phương án đã loại

- Go hoặc Rust cho dịch vụ HTTP: nhân đôi code token và ledger, thêm một toolchain và một bộ luật.
