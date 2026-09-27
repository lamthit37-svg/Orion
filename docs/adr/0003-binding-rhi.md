# ADR 0003 — Binding RHI

- **Trạng thái:** Đề xuất — chờ số đo sàn Android (NGHI-NGO-020) và sàn PC (NGHI-NGO-021)
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.2, §4.3, §5; CLAUDE.md X.13

## Bối cảnh

Ba API đồ hoạ có ba mô hình gắn tài nguyên khác nhau. Mô hình gắn quyết định cách viết shader, cách
render graph cấp phát, và cách GPU-driven rendering (cull bằng compute, indirect draw) truy cập tài
nguyên. Shader chỉ viết một lần bằng HLSL (X.13) rồi dịch sang DXIL, SPIR-V và Metal qua Metal
Shader Converter.

## Quyết định

1. **Bindless ở cả ba API:**
   - D3D12: một descriptor heap CBV/SRV/UAV và một heap sampler cho cả ứng dụng, shader truy cập
     bằng chỉ số qua `ResourceDescriptorHeap[]` và `SamplerDescriptorHeap[]` (Shader Model 6.6);
   - Vulkan: descriptor indexing (lõi từ Vulkan 1.2), một descriptor set lớn cho mỗi loại tài
     nguyên, bật `descriptorBindingPartiallyBound` và `runtimeDescriptorArray`;
   - Metal: argument buffer, theo đúng mô hình top-level argument buffer mà Metal Shader Converter
     sinh ra.
2. **Giao diện shader chung:** mỗi draw và dispatch chỉ nhận một khối hằng nhỏ (root constants,
   push constants, hoặc vùng đầu của argument buffer) chứa các chỉ số tài nguyên. Không có
   descriptor set hay root signature riêng cho từng material.
3. **RHI:** `engine/rhi` cấp chỉ số bindless ổn định cho mỗi tài nguyên; chỉ số được giải phóng qua
   hàng đợi huỷ trễ theo frame (X.13).
4. **Nếu sàn Android không có descriptor indexing:** đường không bindless được ghi rõ vào ADR này
   trước khi viết code cho nó, kèm danh sách pass chạy trên đường đó.

## Hệ quả

- Sàn PC cần Shader Model 6.6 và Agility SDK đi kèm game; sàn này phải khớp khảo sát phần cứng ở
  ADR 0005.
- Sàn Android cần descriptor indexing; nếu số liệu thiết bị thật (ADR 0005) nói khác thì mục 4 được
  viết lại trước khi dựng tier mobile.
- Validation layer (X.13) phải bật kiểm GPU-based cho truy cập bindless ở preset `asan`.

## Phương án đã loại

- Binding truyền thống theo material: nhân số root signature và pipeline layout, khó cho
  GPU-driven rendering.
- Hai mô hình song song (bindless cho PC, truyền thống cho mobile) ngay từ đầu: gấp đôi code
  shader và RHI khi chưa có số liệu chứng minh là cần.
