# ADR 0005 — Sàn thiết bị

- **Trạng thái:** Đề xuất — chưa chốt; mỗi câu hỏi dưới đây chờ số liệu thật
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §4.2, §4.3, §5; CLAUDE.md X.8, X.13; ADR 0003

## Bối cảnh

Sàn thiết bị quyết định API đồ hoạ nào bắt buộc, tier nào phải có, ngân sách bộ nhớ và hiệu năng
đo trên máy nào. Chọn sàn theo cảm tính thì hoặc loại mất người chơi, hoặc kéo cả engine xuống mức
thấp nhất không cần thiết.

## Quyết định

Chưa có quyết định. ADR này chốt **cách** quyết định và những gì đã cố định:

1. **PC có hai tier.** Tier cao là GPU-driven (ARCH §4.3); DX12 Ultimate thuộc tier cao, không
   phải sàn. Sàn PC chốt bằng khảo sát phần cứng thật, và phải có Shader Model 6.6 cho bindless
   (ADR 0003) — điều kiện này chờ số liệu ở NGHI-NGO-021.
2. **Android:** sàn Vulkan (phiên bản, extension, descriptor indexing) chốt bằng số liệu thiết bị
   thật của thị trường mục tiêu (NGHI-NGO-020).
3. **iOS:** chọn giữa chỉ Metal 4 (iOS 26 trở lên, iPhone 12 trở lên) và giữ thêm Metal 3; câu
   hỏi này chờ số liệu thị phần phiên bản iOS của thị trường mục tiêu (NGHI-NGO-022).
4. **Máy tham chiếu:** mỗi dòng ngân sách ở CLAUDE.md X.8 được đo trên một máy tham chiếu cụ thể;
   danh sách máy chốt ở đây khi có (NGHI-NGO-011).
5. **Lớp bộ nhớ mobile:** mỗi lớp thiết bị có ngân sách bộ nhớ riêng, chốt ở đây cùng sàn Android
   và iOS.

## Hệ quả

- Code của tier mobile và tier cao PC không được dựa vào tính năng vượt sàn chưa chốt.
- Khi sàn được chốt, ADR này chuyển sang **Chấp nhận** và ARCH §4.2 được cập nhật trong cùng commit.

## Phương án đã loại

- Chốt sàn theo thông số của máy dev: không đại diện cho người chơi.
