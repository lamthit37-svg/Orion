# ADR 0006 — Định danh

- **Trạng thái:** Chấp nhận
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §5; CLAUDE.md X.7, X.9

## Bối cảnh

Thực thể trong một zone sinh ra và chết đi hàng nghìn lần mỗi phút; tham chiếu tới thực thể đã
chết phải bị phát hiện chứ không trỏ nhầm sang thực thể mới. Nhân vật và vật phẩm thì sống qua
nhiều tiến trình, nhiều lần khởi động và nhiều zone, và là đối tượng của ledger kinh tế.

## Quyết định

1. **`EntityId`:** 64 bit gồm 32 bit chỉ số và 32 bit generation. Chỉ số được tái dùng; generation
   tăng mỗi lần tái dùng, nên id cũ không bao giờ khớp thực thể mới. `EntityId` chỉ sống trong một
   tiến trình: không ghi xuống đĩa, không gửi qua mạng, không gửi sang tiến trình khác.
2. **Tham chiếu thực thể qua mạng:** dùng một id nhân bản do zone cấp, phạm vi một zone, khai trong
   schema (ADR 0004). Id này tách khỏi `EntityId` để bố cục bộ nhớ của server không lộ ra client.
3. **Id bền:** nhân vật và vật phẩm có id 64 bit duy nhất toàn cục, do `persistence` cấp, không bao
   giờ tái dùng. Giá trị 0 là không hợp lệ.
4. **Cấp id bền theo lô:** `persistence` giữ nguồn id trong PostgreSQL và cho zone thuê từng lô id
   liên tiếp; zone cấp id trong lô mà không cần hỏi lại. Zone crash thì phần còn lại của lô bị bỏ,
   để lại khoảng trống, không bao giờ cấp trùng.
5. **Id tài khoản:** do dịch vụ `auth` tạo trong PostgreSQL lúc đăng ký, cũng 64 bit và không tái
   dùng; không đi qua `persistence` vì tài khoản không thuộc mô phỏng.

## Hệ quả

- `engine/core` cung cấp `Handle<T>` có generation (X.7) làm nền cho `EntityId` và các handle khác.
- Protocol có kiểu id nhân bản riêng; test phải chứng minh id nhân bản của thực thể đã chết không
  khớp thực thể mới trong khoảng thời gian client còn giữ tham chiếu.
- Ledger (X.9) chỉ tham chiếu id bền.

## Phương án đã loại

- UUID 128 bit: gấp đôi kích thước trong index, gói tin và ledger mà không cần thiết khi đã có một
  nơi cấp id duy nhất.
- Cấp từng id bền qua một lượt hỏi `persistence`: thêm độ trễ mạng vào mỗi lần tạo vật phẩm.
- Dùng `EntityId` làm id mạng: lộ bố cục bộ nhớ server và buộc hai tiến trình dùng chung không
  gian chỉ số.
