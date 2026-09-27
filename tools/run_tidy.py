#!/usr/bin/env python3
"""Chạy clang-tidy bản ghim trên mọi tệp nguồn của dự án trong compile_commands.json (CLAUDE.md X.1:
mọi phát hiện của clang-tidy chặn merge).

Chỉ kiểm tệp nguồn nằm trong engine/, game/, tools/, tests/; header được kiểm qua
HeaderFilterRegex của .clang-tidy khi tệp nguồn include chúng. Code sinh ra trong out/ được kiểm
gián tiếp qua header của nó.

Gọi tay: `python3.13 tools/run_tidy.py -p out/build/linux` (Windows: `py`, và `-p out/build/dev`).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

import gatelib

CLANG_TIDY_VERSION = "20.1.0"
SOURCE_SUFFIXES = frozenset({".c", ".cpp", ".mm"})
PROJECT_ROOTS = ("engine/", "game/", "tools/", "tests/")
_VERSION_PATTERN = re.compile(r"LLVM version (\d+\.\d+\.\d+)")
_NOISE = re.compile(r"^\d+ warnings? (and \d+ errors? )?generated\.$|^Suppressed \d+ warnings")


def find_clang_tidy() -> str:
    """Chọn clang-tidy đúng bản ghim; ORION_CLANG_TIDY được ưu tiên nếu có."""
    candidates: list[str] = []
    override = os.environ.get("ORION_CLANG_TIDY")
    if override:
        candidates.append(override)
    for name in ("clang-tidy", f"clang-tidy-{CLANG_TIDY_VERSION.split('.')[0]}"):
        found = shutil.which(name)
        if found and found not in candidates:
            candidates.append(found)
    seen: list[str] = []
    for candidate in candidates:
        try:
            output = subprocess.run(
                [candidate, "--version"], capture_output=True, text=True, check=True
            ).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        match = _VERSION_PATTERN.search(output)
        version = match.group(1) if match else "?"
        if version == CLANG_TIDY_VERSION:
            return candidate
        seen.append(f"{candidate} ({version})")
    detail = ", ".join(seen) if seen else "không thấy bản nào"
    raise SystemExit(
        f"run_tidy: cần clang-tidy {CLANG_TIDY_VERSION}; đã thấy: {detail}. Cài bằng "
        f"`pip install clang-tidy=={CLANG_TIDY_VERSION}` hoặc đặt ORION_CLANG_TIDY."
    )


def project_sources(root: pathlib.Path, database: pathlib.Path) -> list[str]:
    """Tệp nguồn của dự án trong compile database, đường dẫn tính từ gốc repo, không trùng lặp."""
    entries = json.loads(database.read_text(encoding="utf-8"))
    root = root.resolve()
    found: set[str] = set()
    for entry in entries:
        path = pathlib.Path(entry["directory"], entry["file"]).resolve()
        try:
            rel = path.relative_to(root).as_posix()
        except ValueError:
            continue
        if rel.startswith(PROJECT_ROOTS) and path.suffix in SOURCE_SUFFIXES:
            found.add(rel)
    return sorted(found)


def run(root: pathlib.Path, build_dir: pathlib.Path, explicit: list[str] | None, jobs: int) -> int:
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(f"run_tidy: không thấy {database}; configure preset trước")
    tidy = find_clang_tidy()
    files = gatelib.select(project_sources(root, database), explicit)

    def one(path: str) -> tuple[str, int, str]:
        result = subprocess.run(
            [tidy, "-p", str(build_dir), "--quiet", path],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        lines = (result.stdout + result.stderr).splitlines()
        text = "\n".join(line for line in lines if not _NOISE.match(line.strip()))
        return path, result.returncode, text

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, code, text in pool.map(one, files):
            if code != 0:
                failed += 1
                print(text or f"{path}: clang-tidy thoát với mã {code}")
    if failed:
        print(f"run_tidy: {failed}/{len(files)} tệp có phát hiện.", file=sys.stderr)
        return 1
    print(f"run_tidy: xanh ({len(files)} tệp).")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("paths", nargs="*", help="chỉ kiểm các tệp hoặc thư mục này")
    parser.add_argument("-p", "--build-dir", type=pathlib.Path, required=True,
                        help="thư mục build chứa compile_commands.json")
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args(argv)
    root = args.root.resolve() if args.root else gatelib.repo_root()
    return run(root, args.build_dir.resolve(), args.paths, max(1, args.jobs))


if __name__ == "__main__":
    sys.exit(main())
