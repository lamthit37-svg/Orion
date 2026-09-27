#!/usr/bin/env python3
"""Cổng kiểm style của Orion (hiến pháp V.5, CLAUDE.md X.1). CMake chạy nó qua target
`orion_check_style`; `engine_core` phụ thuộc target đó nên mọi build đều đi qua cổng.

Kiểm mọi tệp đã track hoặc mới tạo mà chưa bị .gitignore:

- mã hoá: UTF-8 hợp lệ, không BOM;
- xuống dòng: LF, riêng .bat và .cmd là CRLF;
- khoảng trắng: không tab, không khoảng trắng cuối dòng, kết thúc bằng đúng một dấu xuống dòng;
- độ dài: dòng code tối đa 100 ký tự; tệp C++ tối đa 1000 dòng (X.2);
- C++: khớp clang-format bản ghim; header có `#pragma once`, không `using namespace` (X.3);
- comment trong code: việc còn nợ phải mang mã sổ nghi ngờ, không dùng từ phỏng đoán (X.6, X.15).

Gọi tay: `py tools/check_style.py` trên Windows, `python3.13 tools/check_style.py` trên Linux và
Mac. Thêm `--fix` để sửa những gì sửa được tự động (format, khoảng trắng, xuống dòng).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

import gatelib

CLANG_FORMAT_VERSION = "20.1.8"
MAX_LINE_COLUMNS = 100
MAX_CPP_FILE_LINES = 1000

# Vendored code giữ nguyên như thượng nguồn; corpus fuzz là dữ liệu thô.
EXCLUDED_PREFIXES = ("third_party/", "tests/fuzz/corpus/")

BINARY_SUFFIXES = frozenset(
    {
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx2", ".exr", ".hdr", ".psd", ".fbx", ".glb",
        ".bin", ".wav", ".ogg", ".flac", ".bank", ".ttf", ".otf", ".pak", ".ico", ".zip", ".gz",
    }
)
CRLF_SUFFIXES = frozenset({".bat", ".cmd"})
# Tệp mà giới hạn cột có nghĩa. Markdown, JSON, YAML chứa bảng, URL và lệnh dài nên không kiểm cột.
CODE_SUFFIXES = gatelib.CPP_SUFFIXES | frozenset(
    {".py", ".cmake", ".schema", ".sql", ".hlsl", ".hlsli", ".luau", ".lua", ".sh", ".bat", ".ps1"}
)
CODE_NAMES = frozenset({"CMakeLists.txt"})

# Ghép từng mảnh để chính tệp này không tự vi phạm các luật nó kiểm.
_DEBT_WORD = "TO" + "DO"
_DEBT_PATTERN = re.compile(r"\b" + _DEBT_WORD + r"\b(?!\(NGHI-NGO-\d{3,}\))")
_HEDGE_WORDS = ("ch" + "ắc là", "hình" + " như", "có" + " lẽ")
_HEDGE_PATTERN = re.compile("|".join(_HEDGE_WORDS), re.IGNORECASE)
_USING_NAMESPACE = re.compile(r"^\s*using\s+namespace\b", re.MULTILINE)
_PRAGMA_ONCE = re.compile(r"^\s*#\s*pragma\s+once\b", re.MULTILINE)
_VERSION_PATTERN = re.compile(r"clang-format version (\d+\.\d+\.\d+)")


def is_code(path: str) -> bool:
    name = path.rsplit("/", 1)[-1]
    return pathlib.PurePosixPath(path).suffix in CODE_SUFFIXES or name in CODE_NAMES


def is_cpp(path: str) -> bool:
    return pathlib.PurePosixPath(path).suffix in gatelib.CPP_SUFFIXES


def check_text(path: str, raw: bytes) -> list[gatelib.Finding]:
    """Các luật không cần clang-format; trả mọi vi phạm của một tệp."""
    findings: list[gatelib.Finding] = []

    def add(line: int, column: int, rule: str, message: str) -> None:
        findings.append(gatelib.Finding(path, line, column, rule, message))

    if raw.startswith(b"\xef\xbb\xbf"):
        add(1, 1, "encoding/bom", "tệp có BOM; lưu UTF-8 không BOM")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as err:
        line = raw[: err.start].count(b"\n") + 1
        add(line, 1, "encoding/utf8", "không phải UTF-8 hợp lệ")
        return findings
    if not text:
        return findings

    suffix = pathlib.PurePosixPath(path).suffix
    wants_crlf = suffix in CRLF_SUFFIXES
    lines = text.split("\n")
    code = is_code(path)
    for number, line in enumerate(lines[:-1] if text.endswith("\n") else lines, start=1):
        has_cr = line.endswith("\r")
        body = line[:-1] if has_cr else line
        if wants_crlf and not has_cr:
            add(number, len(body) + 1, "whitespace/eol", "tệp .bat và .cmd phải xuống dòng CRLF")
        elif not wants_crlf and "\r" in line:
            add(number, max(line.find("\r"), 0) + 1, "whitespace/eol", "xuống dòng phải là LF")
        if "\t" in body:
            add(number, body.find("\t") + 1, "whitespace/tab", "dùng khoảng trắng thay tab")
        if body != body.rstrip():
            add(number, len(body.rstrip()) + 1, "whitespace/trailing", "khoảng trắng cuối dòng")
        if code and len(body) > MAX_LINE_COLUMNS:
            add(number, MAX_LINE_COLUMNS + 1, "length/line",
                f"dòng dài hơn {MAX_LINE_COLUMNS} ký tự")
        if code:
            debt = _DEBT_PATTERN.search(body)
            if debt:
                add(number, debt.start() + 1, "comment/debt", f"{_DEBT_WORD} phải có mã, ví dụ "
                    f"{_DEBT_WORD}(NGHI-NGO-012)")
            hedge = _HEDGE_PATTERN.search(body)
            if hedge:
                add(number, hedge.start() + 1, "comment/hedge",
                    "không viết phỏng đoán; đo và ghi nguồn, hoặc mở một mục sổ nghi ngờ")

    if not text.endswith("\n"):
        add(len(lines), 1, "whitespace/eof", "tệp phải kết thúc bằng một dấu xuống dòng")
    elif text.endswith("\n\n") or text.endswith("\r\n\r\n"):
        add(len(lines) - 1, 1, "whitespace/eof", "dòng trống thừa ở cuối tệp")

    if is_cpp(path):
        count = text.count("\n")
        if count > MAX_CPP_FILE_LINES:
            add(count, 1, "length/file", f"tệp C++ dài {count} dòng, quá {MAX_CPP_FILE_LINES}; "
                "tách tệp (X.2)")
        if suffix in gatelib.HEADER_SUFFIXES:
            code_only = gatelib.strip_cpp(text, keep_strings=True)
            if suffix != ".inl" and not _PRAGMA_ONCE.search(code_only):
                add(1, 1, "header/pragma-once", "header phải có #pragma once")
            match = _USING_NAMESPACE.search(code_only)
            if match:
                line = code_only.count("\n", 0, match.start()) + 1
                add(line, 1, "header/using-namespace", "không `using namespace` trong header (X.3)")
    return findings


def find_clang_format() -> str:
    """Chọn clang-format đúng bản ghim; ORION_CLANG_FORMAT được ưu tiên nếu có."""
    candidates: list[str] = []
    override = os.environ.get("ORION_CLANG_FORMAT")
    if override:
        candidates.append(override)
    for name in ("clang-format", f"clang-format-{CLANG_FORMAT_VERSION.split('.')[0]}"):
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
        if version == CLANG_FORMAT_VERSION:
            return candidate
        seen.append(f"{candidate} ({version})")
    detail = ", ".join(seen) if seen else "không thấy bản nào"
    raise SystemExit(
        f"check_style: cần clang-format {CLANG_FORMAT_VERSION}; đã thấy: {detail}. Cài bằng "
        f"`py -m pip install clang-format=={CLANG_FORMAT_VERSION}` hoặc đặt ORION_CLANG_FORMAT."
    )


def check_format(
    root: pathlib.Path, path: str, raw: bytes, clang_format: str
) -> list[gatelib.Finding]:
    """So tệp với kết quả clang-format; báo dòng lệch đầu tiên để người đọc biết chỗ cần xem."""
    result = subprocess.run(
        [clang_format, "--style=file", "--fallback-style=none", path],
        cwd=root,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip().splitlines()
        detail = message[0] if message else f"mã thoát {result.returncode}"
        return [gatelib.Finding(path, 1, 1, "format/clang-format", f"clang-format lỗi: {detail}")]
    if result.stdout == raw:
        return []
    original = raw.decode("utf-8", errors="replace").split("\n")
    formatted = result.stdout.decode("utf-8", errors="replace").split("\n")
    line = next(
        (i for i, (a, b) in enumerate(zip(original, formatted), start=1) if a != b),
        min(len(original), len(formatted)),
    )
    return [gatelib.Finding(path, line, 1, "format/clang-format",
                            "chưa đúng .clang-format; chạy check_style.py --fix")]


def fix_file(root: pathlib.Path, path: str, clang_format: str | None) -> None:
    """Sửa những gì sửa máy móc được; phần còn lại vẫn phải sửa tay."""
    full = root / path
    raw = full.read_bytes()
    if b"\0" in raw:
        return
    try:
        text = raw.decode("utf-8").removeprefix("﻿")
    except UnicodeDecodeError:
        return  # Mã hoá sai phải sửa tay; lần kiểm sau sẽ báo.
    eol = "\r\n" if pathlib.PurePosixPath(path).suffix in CRLF_SUFFIXES else "\n"
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    while lines and lines[-1] == "":
        lines.pop()
    fixed = (eol.join(lines) + eol if lines else "").encode("utf-8")
    # Chỉ ghi khi có đổi, để --fix không làm build tăng dần biên dịch lại tệp không đổi.
    if fixed != raw:
        full.write_bytes(fixed)
    if clang_format and is_cpp(path):
        subprocess.run([clang_format, "--style=file", "-i", path], cwd=root, check=True)


class Cache:
    """Nhớ hash của tệp đã xanh để build tăng dần không chạy lại clang-format trên cả repo.

    Khoá cache gồm mã nguồn cổng, .clang-format và bản clang-format; đổi một trong số đó thì mọi
    tệp được kiểm lại.
    """

    def __init__(self, path: pathlib.Path | None, key: str) -> None:
        self._path = path
        self._key = key
        self._entries: dict[str, str] = {}
        if path and path.is_file():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                data = {}
            if isinstance(data, dict) and data.get("key") == key:
                self._entries = dict(data.get("files", {}))

    def fresh(self, rel: str, digest: str) -> bool:
        return self._entries.get(rel) == digest

    def remember(self, passed: dict[str, str], failed: set[str]) -> None:
        """Ghi tệp xanh, xoá tệp đỏ; tệp ngoài lượt kiểm này (khi chỉ kiểm một phần) được giữ."""
        if not self._path:
            return
        self._entries.update(passed)
        for rel in failed:
            self._entries.pop(rel, None)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self._path.with_suffix(".tmp")
        payload = {"key": self._key, "files": self._entries}
        tmp.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(tmp, self._path)


def cache_key(root: pathlib.Path) -> str:
    digest = hashlib.sha256(CLANG_FORMAT_VERSION.encode())
    for part in (pathlib.Path(__file__), pathlib.Path(gatelib.__file__), root / ".clang-format"):
        digest.update(part.read_bytes() if part.is_file() else b"")
    return digest.hexdigest()


def candidate_files(root: pathlib.Path, explicit: list[str] | None) -> list[str]:
    paths = gatelib.select(gatelib.list_source_files(root), explicit)
    return [
        p
        for p in paths
        if not p.startswith(EXCLUDED_PREFIXES)
        and pathlib.PurePosixPath(p).suffix.lower() not in BINARY_SUFFIXES
    ]


def run(root: pathlib.Path, explicit: list[str] | None, cache_path: pathlib.Path | None,
        fix: bool, jobs: int) -> int:
    files = candidate_files(root, explicit)
    needs_format = any(is_cpp(p) for p in files)
    clang_format = find_clang_format() if needs_format else None
    if fix:
        for path in files:
            fix_file(root, path, clang_format)

    cache = Cache(cache_path, cache_key(root))
    findings: list[gatelib.Finding] = []
    passed: dict[str, str] = {}
    failed: set[str] = set()

    def check_one(path: str) -> tuple[str, str, list[gatelib.Finding]]:
        raw = (root / path).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if b"\0" in raw or cache.fresh(path, digest):
            return path, digest, []
        result = check_text(path, raw)
        if clang_format and is_cpp(path) and not result:
            result = check_format(root, path, raw, clang_format)
        return path, digest, result

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, digest, result in pool.map(check_one, files):
            if result:
                findings.extend(result)
                failed.add(path)
            else:
                passed[path] = digest
    cache.remember(passed, failed)
    return gatelib.report(findings, "check_style")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("paths", nargs="*", help="chỉ kiểm các tệp hoặc thư mục này")
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    parser.add_argument("--cache", type=pathlib.Path, help="tệp cache cho build tăng dần")
    parser.add_argument("--fix", action="store_true", help="sửa format và khoảng trắng tại chỗ")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args(argv)
    root = args.root.resolve() if args.root else gatelib.repo_root()
    return run(root, args.paths, args.cache, args.fix, max(1, args.jobs))


if __name__ == "__main__":
    sys.exit(main())
