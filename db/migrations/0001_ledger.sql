-- Ledger ghi kép của kinh tế (CLAUDE.md X.9). Hợp đồng: docs/formats/ledger.md. Chỉ
-- game/server/lib/ledger ghi các bảng này.

-- kind 1: tài khoản giữ, số dư không âm. kind 2: tài khoản ngoài, nguồn và đích của kinh tế, không
-- có số dư và luôn có mã.
CREATE TABLE ledger_accounts (
    id         int8 GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind       int2 NOT NULL CHECK (kind IN (1, 2)),
    code       text UNIQUE CHECK (code ~ '^[a-z][a-z0-9_.]{0,62}$'),
    created_at timestamptz NOT NULL DEFAULT now(),
    -- Đích của khoá ngoại (id, kind) từ ledger_balances.
    UNIQUE (id, kind),
    CHECK (kind = 1 OR code IS NOT NULL)
);

-- Nhật ký: chỉ thêm, không sửa, không xoá.
CREATE TABLE ledger_entries (
    id              int8 GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    idempotency_key bytea NOT NULL UNIQUE CHECK (octet_length(idempotency_key) BETWEEN 1 AND 64),
    reason          int2 NOT NULL CHECK (reason BETWEEN 1 AND 255),
    digest          bytea NOT NULL CHECK (octet_length(digest) = 32),
    created_at      timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE ledger_transfers (
    entry_id     int8 NOT NULL REFERENCES ledger_entries (id),
    ordinal      int2 NOT NULL CHECK (ordinal BETWEEN 0 AND 31),
    from_account int8 NOT NULL REFERENCES ledger_accounts (id),
    to_account   int8 NOT NULL REFERENCES ledger_accounts (id),
    asset        int8 NOT NULL CHECK (asset > 0),
    amount       int8 NOT NULL CHECK (amount BETWEEN 1 AND 1000000000000000),
    PRIMARY KEY (entry_id, ordinal),
    CHECK (from_account <> to_account)
);

-- Số dư của tài khoản giữ; khoá ngoại (account_id, account_kind) cấm dòng của tài khoản ngoài.
CREATE TABLE ledger_balances (
    account_id   int8 NOT NULL,
    account_kind int2 NOT NULL DEFAULT 1 CHECK (account_kind = 1),
    asset        int8 NOT NULL CHECK (asset > 0),
    balance      int8 NOT NULL CHECK (balance >= 0),
    PRIMARY KEY (account_id, asset),
    FOREIGN KEY (account_id, account_kind) REFERENCES ledger_accounts (id, kind)
);

-- Chủ hiện tại của mỗi vật phẩm. version là số lần đã chuyển, từ 1 ở lượt tạo.
CREATE TABLE ledger_items (
    id         int8 PRIMARY KEY CHECK (id > 0),
    asset      int8 NOT NULL CHECK (asset > 0),
    account_id int8 NOT NULL REFERENCES ledger_accounts (id),
    version    int4 NOT NULL CHECK (version >= 1)
);

-- Túi đồ của một tài khoản.
CREATE INDEX ledger_items_account ON ledger_items (account_id);

-- Nhật ký chuyển vật phẩm: mỗi bước (item_id, version) có đúng một dòng.
CREATE TABLE ledger_item_moves (
    entry_id     int8 NOT NULL REFERENCES ledger_entries (id),
    ordinal      int2 NOT NULL CHECK (ordinal BETWEEN 0 AND 31),
    item_id      int8 NOT NULL REFERENCES ledger_items (id),
    version      int4 NOT NULL CHECK (version >= 1),
    from_account int8 NOT NULL REFERENCES ledger_accounts (id),
    to_account   int8 NOT NULL REFERENCES ledger_accounts (id),
    PRIMARY KEY (entry_id, ordinal),
    UNIQUE (item_id, version),
    CHECK (from_account <> to_account)
);
