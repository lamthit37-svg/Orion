"""Tiện ích cho test cổng kiểm: dựng một repo giả trong thư mục tạm và bắt output của cổng."""

from __future__ import annotations

import contextlib
import io
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from collections.abc import Callable

TOOLS_DIR = pathlib.Path(__file__).resolve().parents[1]
REPO_DIR = TOOLS_DIR.parent


class FakeRepo(unittest.TestCase):
    """Mỗi test có một repo git rỗng riêng; không test nào chạm repo thật."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="orion-gate-")
        # resolve(): trên Windows thư mục tạm có thể là tên 8.3 (RUNNER~1), khác đường dẫn thật.
        self.root = pathlib.Path(self._tmp.name).resolve()
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        # Ghi bytes để Windows không tự đổi \n thành \r\n.
        (self.root / "CLAUDE.md").write_bytes("repo giả\n".encode("utf-8"))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, rel: str, content: str | bytes) -> pathlib.Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_bytes(content.encode("utf-8"))
        return path

    def copy_from_repo(self, rel: str) -> None:
        shutil.copyfile(REPO_DIR / rel, self.root / rel)

    def git(self, *args: str) -> None:
        subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)

    @staticmethod
    def capture(fn: Callable[[], int]) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = fn()
        return code, out.getvalue()

    def rules(self, output: str) -> list[tuple[str, str]]:
        """(đường dẫn, luật) của từng dòng vi phạm, bỏ dòng tổng kết."""
        pairs = []
        for line in output.splitlines():
            if ": lỗi [" not in line:
                continue
            path = line.split(":", 1)[0]
            rule = line.split("[", 1)[1].split("]", 1)[0]
            pairs.append((path, rule))
        return pairs
