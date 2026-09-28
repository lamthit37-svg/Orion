# ADR 0007 — Mô hình luồng

- **Trạng thái:** Chấp nhận
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §5, §7; CLAUDE.md X.7

## Bối cảnh

Luồng tạo tuỳ tiện là nguồn race khó tái hiện nhất. CLAUDE.md X.7 chỉ cho tạo luồng trong
`engine/jobs` và các luồng đã được khai. ADR này là nơi khai danh sách đó.

## Quyết định

1. **Chỉ `engine/jobs` tạo luồng hệ điều hành.** Module khác xin một luồng có tên qua API của
   `engine/jobs`; mỗi luồng như vậy phải có mặt trong bảng dưới.
2. **Bảng luồng theo tiến trình:**

   | Tiến trình | Luồng |
   |---|---|
   | mọi tiến trình | luồng chính; một luồng ghi log bất đồng bộ |
   | world, instance | mỗi zone một luồng mô phỏng; job worker; một luồng IO mạng |
   | gateway | các luồng IO mạng; không giữ trạng thái game |
   | persistence | một luồng IO mạng; một luồng nhận lệnh ghi, gom lô, gửi qua libpq pipeline |
   | dịch vụ HTTP | một nhóm luồng IO cố định của Boost.Asio; một nhóm worker cố định chạy handler, với hàng đợi có giới hạn, và pool kết nối DB riêng của các worker (`server/lib/http`, ADR 0015) |
   | client | luồng chính; luồng render; một luồng IO streaming; luồng của FMOD; job worker bằng số core trừ 2 |
   | tool | theo nhu cầu, vẫn tạo qua `engine/jobs` |

   Tiến trình chưa có dòng trong bảng (social, market) được thêm dòng vào đây trước khi viết code
   tạo luồng cho nó.

3. **Tick của zone** gồm các pha: nhận input → movement, combat → AI (tần suất thích ứng) → truy
   vấn physics → AOI → replication. Pha song song chạy trên job worker và gộp kết quả ở cuối pha,
   theo thứ tự không phụ thuộc lịch chạy của job để giữ tất định.
4. **Trao dữ liệu giữa luồng qua hàng đợi.** Luồng IO mạng và luồng mô phỏng trao gói qua hàng đợi
   không khoá; lệnh ghi bền được đẩy bất đồng bộ sang `persistence`. Hot path của mô phỏng không
   dùng khoá (X.7).
5. **Dịch vụ HTTP** không chặn luồng IO: truy vấn DB dùng API bất đồng bộ của libpq hoặc pool kết
   nối riêng.
6. Mỗi kiểu dữ liệu dùng chung giữa luồng ghi cách đồng bộ của nó trong comment (X.7).

## Hệ quả

- `engine/jobs` có API luồng có tên, và mọi luồng ở bảng trên đi qua API đó để có tên trong
  profiler và log.
- Thêm một luồng mới vào bất kỳ tiến trình nào là sửa bảng này trong cùng commit.
- Code chạy nhiều luồng có test dưới TSan (preset `linux-tsan`).

## Phương án đã loại

- Một luồng cho mỗi kết nối: không scale tới 500 người chơi mỗi zone.
- Khoá toàn cục quanh trạng thái zone: phá ngân sách tick ở X.8.
