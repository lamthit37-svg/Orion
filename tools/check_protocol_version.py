#!/usr/bin/env python3
"""Cổng kiểm phiên bản protocol (CLAUDE.md X.10; ADR 0004 mục 4, 5): schema của protocol đổi so với
một commit gốc thì `version` phải tăng, vì client và server lệch phiên bản từ chối nhau.

"Đổi" là đổi code sinh ra, không phải đổi chữ: schema ở commit gốc và ở cây làm việc được đọc
bằng cùng codegen hiện tại, rồi so C++ sinh ra, bỏ dòng ghi tên tệp nguồn và bỏ chính số version.
Sửa comment, chia, gộp hay đổi tên tệp không cần tăng version, vì mỗi lần tăng buộc mọi client cập
nhật; đổi tên, id, kênh, trường, khoảng hay giới hạn tần suất thì cần. Schema của commit gốc mà
codegen hiện tại không đọc được thì coi như đã đổi. version không bao giờ được giảm.

CI truyền commit trước lần đẩy, hay commit gốc của pull request. Khi nó rỗng, toàn số 0 (nhánh mới)
hay không có trong repo, --fallback chọn merge-base của HEAD với nhánh được chỉ.

Gọi tay: `python3.13 tools/check_protocol_version.py --base origin/main`.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import subprocess
import sys

import gatelib
from codegen import cpp
from codegen import schema

PROTOCOL_DIR = "game/shared/protocol"
RULE = "protocol/version"
GATE = "check_protocol_version"
_VERSION_LINE = re.compile(r"^[ \t]*version[ \t]+([0-9]+)", re.MULTILINE)


def _git(root: pathlib.Path, *args: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, check=False)


def resolve_base(root: pathlib.Path, base: str, fallback: str | None) -> str | None:
    """Commit để so: base nếu có trong repo, nếu không thì merge-base của HEAD và fallback."""
    if base.strip("0") and _git(root, "cat-file", "-e", f"{base}^{{commit}}").returncode == 0:
        return base
    if fallback is None:
        return None
    merge = _git(root, "merge-base", "HEAD", fallback)
    return merge.stdout.decode("utf-8").strip() if merge.returncode == 0 else None


def schemas_at(root: pathlib.Path, rev: str) -> dict[str, str]:
    """Các tệp PROTOCOL_DIR/*.schema ở commit rev; rỗng nếu thư mục chưa có ở đó."""
    listing = _git(root, "ls-tree", "--name-only", f"{rev}:{PROTOCOL_DIR}")
    if listing.returncode != 0:
        return {}
    files: dict[str, str] = {}
    for name in listing.stdout.decode("utf-8").splitlines():
        if not name.endswith(".schema"):
            continue
        path = f"{PROTOCOL_DIR}/{name}"
        blob = _git(root, "show", f"{rev}:{path}")
        if blob.returncode != 0:
            raise SystemExit(f"{GATE}: không đọc được {path} ở {rev}")
        files[path] = blob.stdout.decode("utf-8", errors="replace")
    return files


def schemas_in_tree(root: pathlib.Path) -> dict[str, str]:
    directory = root / PROTOCOL_DIR
    return {f"{PROTOCOL_DIR}/{path.name}": gatelib.read_text(path)
            for path in sorted(directory.glob("*.schema")) if path.is_file()}


@dataclasses.dataclass(frozen=True)
class Snapshot:
    version: int
    location: schema.Location
    # C++ sinh ra với version 0 và không có tên tệp nguồn: nghĩa của protocol, không kể version.
    code: str


def snapshot(files: dict[str, str]) -> Snapshot:
    protocol = schema.loads(files)
    neutral = dataclasses.replace(protocol, version=0)
    code = (cpp.generate_header(neutral, "orion::protocol", [])
            + cpp.generate_source(neutral, "orion::protocol", "protocol.hpp", []))
    return Snapshot(protocol.version, protocol.version_location, code)


def declared_version(files: dict[str, str]) -> int | None:
    """Số version của một bộ schema codegen hiện tại không đọc được, nếu khai đúng một lần."""
    found = [int(m.group(1)) for text in files.values() for m in _VERSION_LINE.finditer(text)]
    return found[0] if len(found) == 1 else None


def _short(rev: str) -> str:
    return rev[:12] if re.fullmatch(r"[0-9a-f]{40}", rev) else rev


def compare(base_files: dict[str, str], head: Snapshot, rev: str) -> list[gatelib.Finding]:
    try:
        base: Snapshot | None = snapshot(base_files)
    except schema.SchemaError:
        base = None
    base_version = base.version if base is not None else declared_version(base_files)
    loc = head.location

    def finding(message: str) -> list[gatelib.Finding]:
        return [gatelib.Finding(loc.path, loc.line, loc.column, RULE, message)]

    if base_version is None:
        return finding(f"không đọc được version của schema ở {_short(rev)}; "
                       f"so tay rồi tăng version")
    if head.version < base_version:
        return finding(f"version giảm từ {base_version} xuống {head.version} so với {_short(rev)}")
    changed = base is None or base.code != head.code
    if changed and head.version == base_version:
        return finding(f"protocol đổi so với {_short(rev)} mà version vẫn là {head.version}; "
                       f"tăng version (CLAUDE.md X.10)")
    return []


def run(root: pathlib.Path, rev: str) -> int:
    head_files = schemas_in_tree(root)
    base_files = schemas_at(root, rev)
    if not head_files or not base_files:
        # Chưa có protocol ở một trong hai phía: không có gì để so.
        return gatelib.report([], GATE)
    try:
        head = snapshot(head_files)
    except schema.SchemaError as error:
        print(error.format())
        print(f"{GATE}: schema ở cây làm việc sai, không so được.", file=sys.stderr)
        return 1
    return gatelib.report(compare(base_files, head, rev), GATE)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--base", required=True,
                        help="commit gốc để so (sha, nhánh, tag); rỗng hay toàn 0 thì dùng "
                             "--fallback")
    parser.add_argument("--fallback",
                        help="nhánh để lấy merge-base với HEAD khi --base không dùng được")
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    args = parser.parse_args(argv)
    root = args.root.resolve() if args.root else gatelib.repo_root()
    rev = resolve_base(root, args.base, args.fallback)
    if rev is None:
        tail = f" hay merge-base của HEAD với {args.fallback}" if args.fallback else ""
        raise SystemExit(f"{GATE}: không có commit gốc '{args.base}'{tail}")
    return run(root, rev)


if __name__ == "__main__":
    sys.exit(main())
