"""Phần dùng chung của các cổng kiểm: tìm gốc repo, liệt kê tệp, bóc comment C++, báo lỗi.

Chỉ dùng thư viện chuẩn của Python 3.13, vì cổng phải chạy được trên máy dev Windows, runner Linux
và runner Mac mà không cần cài gói nào (hiến pháp V.5).
"""

from __future__ import annotations

import dataclasses
import os
import pathlib
import subprocess
import sys
from collections.abc import Iterable, Sequence

# Thư mục không bao giờ chứa nguồn: đồ sinh ra, cache của vcpkg, metadata của git.
PRUNED_DIRS = frozenset({".git", "out", "vcpkg_installed", "__pycache__"})

CPP_SUFFIXES = frozenset({".h", ".hpp", ".c", ".cpp", ".inl", ".m", ".mm"})
HEADER_SUFFIXES = frozenset({".h", ".hpp", ".inl"})


@dataclasses.dataclass(frozen=True, order=True)
class Finding:
    """Một vi phạm, in theo dạng `đường/dẫn:dòng:cột: lỗi [luật]: thông điệp` mà editor hiểu."""

    path: str
    line: int
    column: int
    rule: str
    message: str

    def format(self) -> str:
        return f"{self.path}:{self.line}:{self.column}: lỗi [{self.rule}]: {self.message}"


def repo_root(start: pathlib.Path | None = None) -> pathlib.Path:
    """Gốc repo là thư mục cha gần nhất chứa CLAUDE.md và CMakeLists.txt, hoặc chứa .git."""
    here = (start or pathlib.Path(__file__)).resolve()
    for candidate in (here, *here.parents):
        if (candidate / "CLAUDE.md").is_file() or (candidate / ".git").exists():
            return candidate
    raise SystemExit(f"không tìm thấy gốc repo từ {here}")


def _git_files(root: pathlib.Path) -> list[str] | None:
    """Tệp đã track cộng tệp mới chưa bị .gitignore; None nếu không có git hay không phải repo."""
    try:
        result = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=root,
            capture_output=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    names = result.stdout.decode("utf-8").split("\0")
    # Tệp đã xoá trên đĩa nhưng chưa commit vẫn có trong index; bỏ qua vì không còn gì để kiểm.
    return sorted({n for n in names if n and (root / n).is_file()})


def _walk_files(root: pathlib.Path) -> list[str]:
    """Dự phòng khi không có git (ví dụ bản nguồn giải nén): duyệt đĩa, bỏ thư mục sinh ra."""
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in PRUNED_DIRS]
        for name in filenames:
            rel = pathlib.Path(dirpath, name).relative_to(root)
            found.append(rel.as_posix())
    return sorted(found)


def list_source_files(root: pathlib.Path) -> list[str]:
    """Mọi tệp mà cổng phải kiểm, đường dẫn dạng POSIX tính từ gốc repo."""
    files = _git_files(root)
    return files if files is not None else _walk_files(root)


def select(paths: Iterable[str], explicit: Sequence[str] | None) -> list[str]:
    """Lọc theo danh sách đường dẫn người gọi đưa vào (tệp hoặc thư mục), nếu có."""
    if not explicit:
        return list(paths)
    wanted = [p.replace("\\", "/").rstrip("/") for p in explicit]
    return [p for p in paths if any(p == w or p.startswith(w + "/") for w in wanted)]


def read_text(path: pathlib.Path) -> str:
    """Đọc UTF-8 nghiêm ngặt; lỗi giải mã được cổng style báo riêng nên ở đây thay bằng U+FFFD."""
    return path.read_bytes().decode("utf-8", errors="replace")


_RAW_STRING_PREFIXES = frozenset({"R", "u8R", "uR", "UR", "LR"})


def _blank(segment: str) -> str:
    return "".join(ch if ch == "\n" else " " for ch in segment)


def _token_before(text: str, index: int) -> str:
    """Định danh hoặc số liền ngay trước vị trí index (chữ, số, `_`, `'` và `.` của số)."""
    start = index
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] in "_'."):
        start -= 1
    return text[start:index]


def _is_digit_separator(text: str, index: int) -> bool:
    """Dấu `'` là dấu phân cách chữ số (C++14) khi nó nằm trong một số, ví dụ `1'000'000`."""
    token = _token_before(text, index)
    following = text[index + 1] if index + 1 < len(text) else ""
    return bool(token) and token[0].isdigit() and following.isalnum()


def _end_of_line_comment(text: str, start: int) -> int:
    end = start
    while end < len(text) and text[end] != "\n":
        # Dấu \ ở cuối dòng nối comment sang dòng sau.
        if text[end] == "\\" and end + 1 < len(text) and text[end + 1] == "\n":
            end += 2
            continue
        end += 1
    return end


def _end_of_quoted(text: str, start: int) -> int:
    """Vị trí ngay sau dấu nháy đóng; chuỗi hỏng (hết dòng trước khi đóng) dừng ở cuối dòng."""
    quote = text[start]
    end = start + 1
    while end < len(text) and text[end] != quote and text[end] != "\n":
        end += 2 if text[end] == "\\" else 1
    return min(end + 1, len(text))


def strip_cpp(text: str, *, keep_strings: bool) -> str:
    """Thay comment (và chuỗi, nếu keep_strings=False) bằng khoảng trắng, giữ nguyên số dòng.

    Máy trạng thái lặp, không đệ quy. Hiểu comment `//` và `/* */`, chuỗi và ký tự có tiền tố
    (`u8"..."`, `L'x'`), chuỗi thô `R"delim(...)delim"`, dấu nối dòng, và dấu phân cách chữ số.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = _end_of_line_comment(text, i)
            out.append(_blank(text[i:end]))
        elif ch == "/" and nxt == "*":
            close = text.find("*/", i + 2)
            end = n if close < 0 else close + 2
            out.append(_blank(text[i:end]))
        elif ch == '"' and _token_before(text, i) in _RAW_STRING_PREFIXES:
            open_paren = text.find("(", i + 1)
            delim = text[i + 1 : open_paren] if open_paren >= 0 else ""
            close = text.find(")" + delim + '"', open_paren + 1) if open_paren >= 0 else -1
            end = n if close < 0 else close + len(delim) + 2
            segment = text[i:end]
            out.append(segment if keep_strings else '"' + _blank(segment[1:]))
        elif ch == '"' or (ch == "'" and not _is_digit_separator(text, i)):
            end = _end_of_quoted(text, i)
            segment = text[i:end]
            if keep_strings or len(segment) < 2:
                out.append(segment)
            else:
                out.append(ch + _blank(segment[1:-1]) + segment[-1])
        else:
            out.append(ch)
            end = i + 1
        i = end
    return "".join(out)


def report(findings: Sequence[Finding], gate: str) -> int:
    """In vi phạm theo thứ tự ổn định và trả mã thoát: 0 xanh, 1 có vi phạm."""
    for finding in sorted(findings):
        print(finding.format())
    if findings:
        print(f"{gate}: {len(findings)} vi phạm.", file=sys.stderr)
        return 1
    print(f"{gate}: xanh.")
    return 0
