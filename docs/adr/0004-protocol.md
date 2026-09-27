# ADR 0004 — Protocol

- **Trạng thái:** Chấp nhận
- **Ngày:** 2026-09-27
- **Liên quan:** ARCH §3, §5; CLAUDE.md X.4, X.9, X.10

## Bối cảnh

Client và server, và các server với nhau, trao đổi tin nhắn qua mạng và qua Redis Streams. Viết
tay serializer cho từng tin nhắn thì dễ lệch giữa hai đầu, khó giới hạn kích thước, và mỗi parser
viết tay là một bề mặt tấn công. Băng thông mỗi client có trần ở X.8.

## Quyết định

1. **Nguồn duy nhất:** mọi tin nhắn, kể cả tin nhắn giữa các server và sự kiện qua Redis Streams,
   được định nghĩa trong `game/shared/protocol/*.schema`. Codegen trong `tools/codegen/` sinh ra
   kiểu C++, bộ mã hoá và bộ giải mã vào `out/`; code sinh ra không được commit hay sửa tay.
2. **Định dạng trên dây:** bit-packed, thứ tự little-endian tường minh. Số nguyên khai khoảng giá
   trị để codegen chọn số bit. Mọi mảng và chuỗi có độ dài tối đa. Mọi số thực gửi qua mạng khai
   lượng tử hoá (khoảng và độ chính xác). Enum có kiểu nền và giá trị tường minh. Codegen từ chối
   schema thiếu bất kỳ khai báo nào ở trên.
3. **Kênh:** mỗi tin nhắn thuộc đúng một kênh, khai trong schema: `unreliable`, `sequenced`,
   `reliable_ordered` hoặc `reliable_unordered`.
4. **Phiên bản:** schema khai `kProtocolVersion` kiểu `u32`. Đổi bất kỳ schema nào thì phải tăng
   số này; CI so schema với nhánh gốc và chặn nếu quên tăng.
5. **Không tương thích ngược:** bắt tay mang `kProtocolVersion`; lệch phiên bản thì server từ chối
   bằng một gói không lớn hơn gói yêu cầu (chống khuếch đại, X.9) và client buộc phải cập nhật.
6. **Giải mã là hàm toàn phần:** mọi dãy byte cho ra hoặc một tin nhắn hợp lệ, hoặc một `Error`;
   không bao giờ UB, không bao giờ assert trên dữ liệu vào. Mỗi điểm vào giải mã có một fuzz target
   (X.4).
7. **Kiểm rồi mới áp:** bộ giải mã kiểm toàn bộ tin nhắn trước khi trả về; handler không đổi trạng
   thái trước khi kiểm xong (X.10).

## Hệ quả

- `docs/formats/protocol.md` mô tả ngôn ngữ schema và định dạng trên dây; đổi định dạng thì sửa
  file đó trong cùng commit (X.15).
- Codegen có test riêng cho từng loại lỗi schema và cho tính khứ hồi mã hoá rồi giải mã.
- Mỗi bản phát hành là một phiên bản protocol; cụm server và client luôn được cập nhật cùng nhau.

## Phương án đã loại

- Protobuf, FlatBuffers: không bit-packed, không có lượng tử hoá, và mang thêm một dependency lớn
  vào mọi sản phẩm.
- Tương thích ngược theo trường tuỳ chọn: tăng kích thước gói và số nhánh cần fuzz, trong khi MMO
  có thể buộc client cập nhật.
- `memcpy` struct: cấm bởi CLAUDE.md X.7.
