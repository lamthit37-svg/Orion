#!/usr/bin/env python3
"""Cổng kiểm tệp nguồn bị .gitignore nuốt (CLAUDE.md X.1).

Một tệp nguồn nằm trong cây repo mà khớp một luật của .gitignore thì build trên máy này vẫn xanh,
nhưng tệp đó không bao giờ được commit: máy khác và CI sẽ đỏ, hoặc tệ hơn là build một bản khác.
Cổng này duyệt cây thư mục (trừ out/, .git/ và thư mục cục bộ của editor), hỏi git xem tệp nào bị
bỏ qua, và báo mọi tệp có đuôi của tệp nguồn.

Gọi tay: `py tools/check_tracked.py` trên Windows, `python3.13 tools/check_tracked.py` trên Linux.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys

import gatelib

SOURCE_SUFFIXES = gatelib.CPP_SUFFIXES | frozenset(
    {
        ".py", ".cmake", ".json", ".schema", ".sql", ".hlsl", ".hlsli", ".luau", ".lua", ".md",
        ".yaml", ".yml", ".toml", ".txt", ".sh", ".bat", ".cmd", ".ps1", ".gradle", ".kts",
        ".xml", ".plist", ".in",
    }
)
SOURCE_NAMES = frozenset({"CMakeLists.txt", "Dockerfile", "CMakePresets.json"})
# Thư mục ẩn ở gốc là của editor hoặc công cụ cục bộ, cố ý bị bỏ qua; riêng .github là nguồn.
KEPT_HIDDEN = frozenset({".github"})


def candidate_files(root: pathlib.Path) -> list[str]:
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = pathlib.Path(dirpath).relative_to(root)
        at_root = rel_dir == pathlib.Path(".")
        dirnames[:] = sorted(
            d
            for d in dirnames
            if d not in gatelib.PRUNED_DIRS
            and not (at_root and d.startswith(".") and d not in KEPT_HIDDEN)
        )
        for name in filenames:
            suffix = pathlib.PurePosixPath(name).suffix
            if suffix in SOURCE_SUFFIXES or name in SOURCE_NAMES:
                found.append((rel_dir / name).as_posix())
    return sorted(found)


def ignored(root: pathlib.Path, paths: list[str]) -> list[str]:
    """Tệp chưa track mà .gitignore bỏ qua. Tệp đã track thì git không áp luật ignore nữa."""
    if not paths:
        return []
    result = subprocess.run(
        ["git", "check-ignore", "--stdin", "-z"],
        cwd=root,
        input="\0".join(paths).encode("utf-8") + b"\0",
        capture_output=True,
        check=False,
    )
    # Mã 1 nghĩa là không tệp nào bị bỏ qua; mã khác 0 và 1 là lỗi của chính git.
    if result.returncode not in (0, 1):
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise SystemExit(f"check_tracked: git check-ignore lỗi: {message}")
    return sorted(p for p in result.stdout.decode("utf-8").split("\0") if p)


def run(root: pathlib.Path) -> int:
    findings = [
        gatelib.Finding(path, 1, 1, "tracked/ignored",
                        "tệp nguồn khớp .gitignore nên sẽ không được commit; sửa .gitignore hoặc "
                        "chuyển tệp ra out/")
        for path in ignored(root, candidate_files(root))
    ]
    return gatelib.report(findings, "check_tracked")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    args = parser.parse_args(argv)
    root = args.root.resolve() if args.root else gatelib.repo_root()
    return run(root)


if __name__ == "__main__":
    sys.exit(main())
