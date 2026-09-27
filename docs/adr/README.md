# ADR — quyết định trụ cột

Mỗi quyết định mà làm sai thì phải đập lại cả hệ được ghi thành một file `NNNN-ten.md` ở thư mục
này (ARCH §5). Đổi một trụ cột thì viết ADR mới trước, sửa ARCH sau, rồi mới sửa code.

## Quy trình

1. Chép mẫu dưới đây vào file mới, lấy số kế tiếp, tên file viết thường, không dấu, nối bằng `-`.
2. Trạng thái bắt đầu là **Đề xuất**. Chỉ người quyết mới chuyển sang **Chấp nhận**.
3. ADR đã chấp nhận không bị sửa nội dung quyết định. Muốn đổi thì viết ADR mới và ghi
   **Thay bởi NNNN** vào ADR cũ.
4. Mọi giả định chưa đo trong ADR là một mục trong `docs/NGHI-NGO.md`, được trỏ tới bằng mã.

## Mẫu

```markdown
# ADR NNNN — Tên quyết định

- **Trạng thái:** Đề xuất | Chấp nhận | Thay bởi NNNN
- **Ngày:** YYYY-MM-DD
- **Liên quan:** mục ARCH, luật CLAUDE.md, mục sổ nghi ngờ

## Bối cảnh

Vấn đề gì buộc phải quyết, ràng buộc nào đang có.

## Quyết định

Chọn gì, nói đủ chính xác để code và test kiểm được.

## Hệ quả

Cái được, cái mất, việc phải làm tiếp.

## Phương án đã loại

Mỗi phương án một dòng, kèm lý do loại.
```

## Danh sách

| Số | Tên | Trạng thái |
|---|---|---|
| 0001 | [Hệ toạ độ](0001-he-toa-do.md) | Chấp nhận |
| 0002 | [Thời gian](0002-thoi-gian.md) | Chấp nhận |
| 0003 | [Binding RHI](0003-binding-rhi.md) | Đề xuất |
| 0004 | [Protocol](0004-protocol.md) | Chấp nhận |
| 0005 | [Sàn thiết bị](0005-san-thiet-bi.md) | Đề xuất |
| 0006 | [Định danh](0006-dinh-danh.md) | Chấp nhận |
| 0007 | [Mô hình luồng](0007-mo-hinh-luong.md) | Chấp nhận |
| 0008 | [Một ngôn ngữ](0008-mot-ngon-ngu.md) | Chấp nhận |
| 0009 | [CI trên GitHub Actions](0009-ci-github-actions.md) | Đề xuất |
