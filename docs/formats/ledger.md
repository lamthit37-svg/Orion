# Ledger — sổ ghi kép của kinh tế

Mọi thay đổi về vật phẩm và tiền là một bút toán ghi kép trong một transaction của PostgreSQL, kèm
khoá idempotency (CLAUDE.md X.9). Chỉ `game/server/lib/ledger` ghi các bảng ở đây; store và
persistence gọi cùng một bản code (ARCH §4.6, ADR 0008). Crash hay retry ở bất kỳ điểm nào cũng
không được nhân bản vật phẩm hay tiền: test tiêm lỗi của module chứng minh điều đó (mục "Test").

Đổi bất kỳ điều gì ở đây là đổi hợp đồng: sửa tài liệu này, code và một migration mới trong cùng
commit (CLAUDE.md X.15). Bảng được tạo ở `db/migrations/0001_ledger.sql`.

## Khái niệm

- **Tài khoản** giữ tài sản và vật phẩm. Có hai loại, không bao giờ đổi:
  - *giữ* (`kind = 1`): ví, túi đồ của nhân vật, ký quỹ. Số dư không bao giờ âm.
  - *ngoài* (`kind = 2`): nguồn và đích của kinh tế, ví dụ rơi đồ, cửa hàng NPC, store, phí. Không
    có số dư: tài sản sinh ra từ đó và biến mất vào đó. Không lưu số dư cho tài khoản ngoài nên
    không có dòng nào bị mọi giao dịch cùng tranh khoá. Mọi tài khoản ngoài có mã.
- **Mã tài khoản** (tuỳ chọn với tài khoản giữ): 1 tới 63 byte, chữ thường `a`–`z` đứng đầu, sau đó
  chữ thường, chữ số, `_` và `.`, ví dụ `store.revenue`. Dịch vụ tìm tài khoản hệ thống của nó
  bằng mã.
- **Tài sản** (`asset`): thứ đếm được và gộp được, như tiền hay nguyên liệu chồng được; là id của
  định nghĩa trong `data/`, lớn hơn 0. Ledger không phân biệt tiền với nguyên liệu.
- **Vật phẩm** (`item`): một vật duy nhất có id bền 64 bit do persistence cấp (ADR 0006), thuộc một
  định nghĩa (`asset`). Mỗi vật phẩm có đúng một chủ tại mọi thời điểm, và một số thứ tự lần chuyển
  (`version`) tăng 1 mỗi lần chuyển.
- **Bút toán** (`entry`): nhóm các dòng chuyển tài sản và lượt chuyển vật phẩm được áp nguyên khối,
  kèm khoá idempotency và lý do.
  - *Dòng chuyển* (`transfer`): `amount` đơn vị `asset` đi từ tài khoản `from` sang tài khoản `to`.
  - *Lượt chuyển vật phẩm* (`item move`): vật phẩm `item` đi từ `from` sang `to`. Đi từ tài khoản
    ngoài là tạo vật phẩm mới; đi vào tài khoản ngoài là huỷ nó. Vật phẩm đã huỷ vẫn nằm trong bảng
    với chủ là tài khoản ngoài đó, và không bao giờ sống lại: id của nó không cấp lại được.

Mỗi dòng và mỗi lượt có đúng hai đầu, nên tổng mọi thay đổi của một tài sản luôn bằng 0: đó là ghi
kép, đúng theo cấu trúc bảng chứ không nhờ một phép kiểm tổng.

## Bảng

| Bảng | Cột | Luật |
|---|---|---|
| `ledger_accounts` | `id int8` | identity, khoá chính |
| | `kind int2` | 1 hay 2 |
| | `code text` | NULL, hay duy nhất và đúng dạng ở trên; tài khoản ngoài phải có |
| | `created_at timestamptz` | lúc tạo |
| `ledger_entries` | `id int8` | identity, khoá chính |
| | `idempotency_key bytea` | 1 tới 64 byte, duy nhất |
| | `reason int2` | 1 tới 255, xem "Lý do" |
| | `digest bytea` | 32 byte, xem "Digest" |
| | `created_at timestamptz` | lúc ghi |
| `ledger_transfers` | `entry_id int8`, `ordinal int2` | khoá chính; `ordinal`: vị trí, từ 0 |
| | `from_account int8`, `to_account int8` | khác nhau, trỏ `ledger_accounts` |
| | `asset int8` | lớn hơn 0 |
| | `amount int8` | 1 tới 10¹⁵ |
| `ledger_balances` | `account_id int8`, `asset int8` | khoá chính; chỉ tài khoản giữ (khoá ngoại tới `(id, kind)`) |
| | `balance int8` | không âm |
| `ledger_items` | `id int8` | khoá chính, lớn hơn 0 |
| | `asset int8` | lớn hơn 0, không đổi |
| | `account_id int8` | chủ hiện tại |
| | `version int4` | số lần đã chuyển, từ 1 (lượt tạo) |
| `ledger_item_moves` | `entry_id int8`, `ordinal int2` | khoá chính |
| | `item_id int8`, `version int4` | duy nhất: mỗi bước của một vật phẩm được ghi đúng một lần |
| | `from_account int8`, `to_account int8` | khác nhau |

`ledger_entries`, `ledger_transfers`, `ledger_item_moves` là nhật ký: chỉ thêm, không sửa, không
xoá. `ledger_balances` và `ledger_items` là trạng thái suy ra được từ nhật ký; soát sổ (mục "Soát")
kiểm hai thứ khớp nhau.

## Ghi một bút toán

Bên gọi mở transaction, gọi `post` một hay nhiều lần, cùng các ghi khác của nó (ví dụ đơn hàng của
store), rồi commit. Mọi ghi của một bút toán nằm trong transaction đó, nên hoặc có hết, hoặc không
có gì. `post` theo thứ tự kiểm rồi mới áp (CLAUDE.md X.10):

1. Kiểm bút toán, không chạm DB (mục "Giới hạn"): lỗi `InvalidArgument` hay `OutOfRange`.
2. Đọc loại của mọi tài khoản có mặt. Tài khoản không có: `NotFound`. Dòng hay lượt có cả hai đầu
   là tài khoản ngoài: `FailedPrecondition`.
3. Thêm dòng `ledger_entries`; khoá đã có thì không thêm (`ON CONFLICT DO NOTHING`), sang bước 7.
4. Cộng dồn thay đổi của mỗi cặp (tài khoản giữ, tài sản) trong cả bút toán, rồi áp theo thứ tự
   tăng dần của cặp: trừ chỉ khi số dư đủ (`balance >= amount` trong cùng câu `UPDATE`), không đủ
   thì `FailedPrecondition`. Bút toán hợp lệ khi số dư cuối cùng không âm, nên một tài khoản nhận
   rồi chuyển đi trong cùng bút toán không cần có sẵn số dư. Cộng tràn `int8`: `OutOfRange`.
5. Chuyển vật phẩm theo thứ tự tăng dần của id. Từ tài khoản giữ: đổi chủ và tăng `version` chỉ khi
   chủ hiện tại là `from` và định nghĩa khớp; vật phẩm không có là `NotFound`, khác chủ hay khác
   định nghĩa là `FailedPrecondition`. Từ tài khoản ngoài: tạo vật phẩm với `version = 1`; id đã có
   là `AlreadyExists`. Mỗi lượt thêm một dòng `ledger_item_moves` với `version` mới.
6. Thêm các dòng `ledger_transfers`. Trả id của bút toán, `replayed = false`.
7. Khoá đã có: đọc bút toán cũ. Cùng digest thì trả id của nó với `replayed = true` và không áp gì
   nữa; khác digest thì `AlreadyExists`: khoá đã dùng cho một bút toán khác.

Mọi lỗi sau bước 1 đều để lại transaction không dùng tiếp được: bên gọi rollback. Lỗi của server
(`Aborted`, `Unavailable`, `DeadlineExceeded`, ...) đi nguyên lên như `server_db` trả.

Thứ tự khoá cố định (số dư theo cặp tăng dần, rồi vật phẩm theo id tăng dần) làm hai bút toán đơn
lẻ không bao giờ deadlock với nhau. Khoá mà khoá ngoại lấy trên dòng được trỏ tới (`FOR KEY SHARE`)
không xung đột với các câu `UPDATE` của ledger, vì chúng không đổi cột khoá nào (tài liệu
PostgreSQL, mục "Row-Level Locks"). Transaction ghi nhiều bút toán vẫn có thể deadlock; PostgreSQL
huỷ một bên (`Aborted`) và bên đó chạy lại.

### Mức cô lập

Khuyên dùng `ReadCommitted`. Mỗi câu trừ số dư hay chuyển vật phẩm là một `UPDATE` có điều kiện
trên đúng một dòng. Ở mức này, khi hai transaction cùng sửa một dòng, bên sau chờ bên trước kết
thúc rồi kiểm lại điều kiện trên bản mới nhất của dòng (tài liệu PostgreSQL, mục "Read Committed
Isolation Level"), nên hai lần tiêu cùng một số dư không bao giờ cùng thành công. Hai transaction
cùng thêm một khoá idempotency thì bên sau chờ bên trước: bên trước commit thì bên sau thấy khoá đã
có và trả `replayed`; bên trước rollback thì bên sau áp.

Ở `RepeatableRead` và `Serializable`, cùng những xung đột đó thành lỗi `Aborted` (SQLSTATE 40001):
bên gọi chạy lại cả transaction.

### Retry

Commit trả `Unavailable` hay `DeadlineExceeded` thì bên gọi không biết transaction đã commit hay
chưa. Nó chạy lại toàn bộ transaction với đúng bút toán cũ và khoá cũ, trên kết nối khác nếu cần:
nếu lần trước đã commit, `post` trả `replayed`; nếu chưa, lần này áp. Cùng cách với `Aborted`. Vì
vậy bên gọi phải dựng lại được đúng bút toán khi retry, và không được sinh khoá mới cho một lần
retry.

## Khoá idempotency

1 tới 64 byte, duy nhất trên toàn bộ ledger. Bên gọi dựng khoá sao cho nó duy nhất cho mỗi thao tác
logic, và không ai khác chọn hay đoán được khoá của người khác: ví dụ store ghép id tài khoản người
chơi với id request do client sinh, để client này không chặn được request của client kia bằng cách
dùng trước khoá của nó. Mỗi nguồn nên có tiền tố riêng (`store:`, `zone:`).

## Digest

BLAKE2b 256 bit (`engine/crypto/hash.hpp`) của nội dung bút toán, trừ khoá, theo đúng thứ tự bên gọi
đưa. Mọi số nguyên là little-endian:

| Trường | Kiểu |
|---|---|
| nhãn `orion.ledger.entry.v1` (21 byte ASCII, không NUL) | byte |
| lý do | `u8` |
| số dòng chuyển | `u8` |
| mỗi dòng: `from`, `to`, `asset`, `amount` | 4 × `i64` |
| số lượt chuyển vật phẩm | `u8` |
| mỗi lượt: `item`, `asset`, `from`, `to` | 4 × `i64` |

Digest chỉ để phát hiện một khoá bị dùng lại cho nội dung khác. Retry phải đưa cùng các dòng theo
cùng thứ tự.

## Giới hạn

| Thứ | Giới hạn | Lỗi khi vượt |
|---|---|---|
| Khoá idempotency | 1 tới 64 byte | `InvalidArgument` |
| Dòng chuyển mỗi bút toán | 0 tới 32 | `InvalidArgument` |
| Lượt chuyển vật phẩm mỗi bút toán | 0 tới 32; tổng với dòng chuyển ít nhất 1 | `InvalidArgument` |
| `amount` | 1 tới 10¹⁵ | `OutOfRange` |
| Id tài khoản, tài sản, vật phẩm | lớn hơn 0 | `InvalidArgument` |
| Hai đầu của một dòng hay một lượt | khác nhau | `InvalidArgument` |
| Một vật phẩm trong một bút toán | tối đa một lượt | `InvalidArgument` |
| Lý do | một giá trị của bảng dưới | `InvalidArgument` |

32 dòng mỗi loại đủ cho mọi luồng kinh tế đã biết: một lần trao đổi hai người với cả chục vật phẩm,
một lần chế tạo tiêu vài nguyên liệu. 10¹⁵ nhân 32 dòng vẫn xa giới hạn của `int8`, nên cộng dồn
trong bước 4 không bao giờ tràn.

## Lý do

`reason` ghi ra đĩa: giá trị không bao giờ đổi, chỉ thêm giá trị mới (CLAUDE.md X.3).

| Giá trị | Tên | Dùng cho |
|---|---|---|
| 1 | `Grant` | vận hành cấp hay thu hồi: GM, bồi thường |
| 2 | `Purchase` | mua trong store |
| 3 | `Loot` | rơi đồ, rương, phần thưởng |
| 4 | `Trade` | trao đổi giữa hai người chơi |
| 5 | `Vendor` | mua bán với NPC |
| 6 | `Consume` | dùng, phá vật phẩm; phí: tài sản ra khỏi kinh tế |

## Soát

`audit` đọc toàn bộ nhật ký, O(số dòng), cho công cụ vận hành và cho test. Nó đếm:

- cặp (tài khoản giữ, tài sản) có `balance` khác tổng các dòng chuyển vào trừ các dòng chuyển ra,
  kể cả cặp có dòng chuyển mà không có dòng `ledger_balances`;
- vật phẩm có chủ khác `to_account` của lượt có `version` bằng `version` của nó, kể cả khi không có
  lượt đó;
- lượt chuyển đứt chuỗi: lượt đầu tiên (`version = 1`) không đi từ tài khoản ngoài, lượt sau không
  đi từ `to_account` của lượt ngay trước, hay lượt có `version` lớn hơn `version` của vật phẩm.

Sổ lành khi cả ba bằng 0.

## Test

`game/server/lib/ledger/tests/` chạy trên PostgreSQL thật. Test tiêm lỗi (CLAUDE.md X.9) cài trigger
chỉ có trong schema của test lên mọi bảng ledger, và làm hỏng lần ghi dòng thứ k của một bút toán
cho mọi k, bằng hai cách: báo lỗi (transaction rollback), và tự ngắt kết nối của chính phiên đó
(như tiến trình gọi crash giữa chừng). Thêm hai điểm: lúc COMMIT (trigger hoãn tới commit), và sau
khi post xong mà trước commit (một phiên khác ngắt kết nối). Sau mỗi lần hỏng, sổ phải y như trước
và soát lành; retry với cùng khoá phải áp đúng một lần; retry thêm nữa trả `replayed`.

Test đồng thời chạy phiên thứ hai trên luồng riêng và chỉ đi tiếp khi `pg_locks` cho thấy phiên đó
đang chờ khoá, nên thứ tự xen kẽ cố định: hai lần tiêu cùng một số dư, hai lần thử cùng một khoá
(lần trước commit hay rollback), khoá của bút toán ở `RepeatableRead`, deadlock giữa hai transaction
nhiều bút toán, hai bút toán chiều ngược nhau không deadlock (một trigger giữ phiên thứ nhất lại
ngay sau dòng số dư đầu tiên nó khoá), và vật phẩm được trả về đúng chủ giữa câu chuyển không khớp
và câu đọc lại (`Aborted`).
