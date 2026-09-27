# ADR 0002 — Thời gian

- **Trạng thái:** Chấp nhận
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §5, §7; CLAUDE.md X.8, X.10, X.11

## Bối cảnh

Mô phỏng tất định cần một trục thời gian rời rạc mà mọi bên đồng ý. Đồng hồ tường của client không
đáng tin, đồng hồ tường của server nhảy khi NTP chỉnh. Băng thông mỗi client có trần (X.8), nên
không thể gửi mọi thứ ở mọi tick.

## Quyết định

1. **Tick:** mô phỏng server chạy cố định 50 Hz, mỗi tick 20 ms. Hằng số nằm trong `game/shared`.
2. **Kiểu `Tick`:** kiểu mạnh bọc `u64`, đếm từ 0 khi zone khởi động. 64 bit để không bao giờ
   tràn trong đời một zone. Trên dây, tick được mã hoá theo schema (ADR 0004): bản đầy đủ trong bắt
   tay, bản rút gọn quay vòng kèm luật khôi phục trong gói thường.
3. **Nguồn thời gian của mô phỏng:** chỉ có `Tick`. `game/shared` không đọc đồng hồ (X.11); thời
   gian đi vào dưới dạng số tick và độ dài tick.
4. **Snapshot tách khỏi tick:** tần suất gửi trạng thái cho từng client là thích ứng: mỗi thực thể
   có ưu tiên tích luỹ theo khoảng cách và mức quan trọng, mỗi client có ngân sách băng thông
   (X.8). Thực thể gần có thể được gửi mỗi tick, thực thể xa thưa hơn.
5. **Client:** thực thể của người khác được nội suy với độ trễ cố định tính bằng tick. Nhân vật
   của chính client được dự đoán bằng cùng code `game/shared`, rồi đối chiếu với trạng thái server.
6. **Server không tin đồng hồ client** (X.10). Input của client mang số tick client ước lượng; server
   xếp input vào tick của chính nó qua một bộ đệm jitter nhỏ, không bao giờ lùi tick.
7. **Lịch tick:** vòng lặp zone dùng đồng hồ đơn điệu, được tiêm vào để test. Zone chậm thì chạy
   bù các tick liên tiếp, không bỏ tick nào, và phát metric khi độ trễ vượt ngưỡng.
8. **Đồng hồ tường:** chỉ dùng cho hạn của token, log, dấu thời gian trong DB và kiểm toán kinh tế;
   luôn là UTC, đơn vị giây hoặc micro giây kể từ Unix epoch. Các server đồng bộ bằng NTP; kiểm hạn
   token có dung sai lệch đồng hồ khai trong cấu hình.

## Hệ quả

- `engine/core` cung cấp giao diện đồng hồ (đơn điệu và tường) có bản giả cho test (X.4: test
  không dùng đồng hồ thật).
- Protocol có luật mã hoá tick rút gọn và test quay vòng.
- Độ trễ nội suy và kích thước bộ đệm jitter là tham số cần đo; con số ban đầu là giả định
  (NGHI-NGO-011).

## Phương án đã loại

- Tick biến thiên theo tải: phá tất định và replay.
- Gửi snapshot mỗi tick cho mọi thực thể trong tầm nhìn: vượt ngân sách mạng ở X.8.
- `Tick` 32 bit: tràn sau khoảng 2,7 năm ở 50 Hz, buộc mọi nơi xử lý quay vòng.
