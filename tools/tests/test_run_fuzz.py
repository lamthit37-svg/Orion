"""Test run_fuzz: tìm target, dựng lệnh libFuzzer, và luồng fuzz rồi thu gọn corpus với một chương
trình giả đóng vai libFuzzer. Fuzz thật do job fuzz đêm của CI chạy."""

from __future__ import annotations

import os
import pathlib
import stat
import sys
import tempfile
import textwrap
import unittest

import run_fuzz
from tests.support import FakeRepo

# Chương trình giả: ghi lại tham số; khi -merge=1 thì chép input từ thư mục nguồn sang thư mục
# đích; thoát bằng mã trong tệp exit_code cạnh nó (không có tệp thì 0).
FAKE_FUZZER = textwrap.dedent(
    """\
    import pathlib
    import shutil
    import sys

    here = pathlib.Path(__file__).resolve().parent
    with open(here / "calls.txt", "a", encoding="utf-8") as log:
        log.write(" ".join(sys.argv[1:]) + "\\n")
    if "-merge=1" in sys.argv:
        dirs = [pathlib.Path(a) for a in sys.argv[1:] if not a.startswith("-")]
        for item in dirs[1].iterdir():
            shutil.copy(item, dirs[0] / item.name)
    code_file = here / "exit_code"
    sys.exit(int(code_file.read_text()) if code_file.exists() else 0)
    """
)


def make_executable(path: pathlib.Path) -> None:
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class DiscoverTest(unittest.TestCase):
    def test_finds_only_executable_fuzz_targets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = pathlib.Path(tmp)
            fuzz_dir = build / "tests" / "fuzz"
            fuzz_dir.mkdir(parents=True)
            for name in ("fuzz_b", "fuzz_a", "fuzz_not_executable", "helper"):
                (fuzz_dir / name).write_text("")
            make_executable(fuzz_dir / "fuzz_a")
            make_executable(fuzz_dir / "fuzz_b")
            make_executable(fuzz_dir / "helper")
            (fuzz_dir / "fuzz_dir").mkdir()
            if os.name == "nt":
                self.skipTest("Windows không có bit thực thi")
            self.assertEqual(run_fuzz.discover(build), ["a", "b"])

    def test_missing_directory_means_no_targets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(run_fuzz.discover(pathlib.Path(tmp)), [])


class CommandTest(unittest.TestCase):
    # Giá trị mong đợi đi qua str(Path) vì Windows viết đường dẫn bằng dấu \.
    BINARY = pathlib.Path("/b/fuzz_x")
    WORK = pathlib.Path("/w/x")

    def test_fuzz_command_writes_only_to_work_corpus(self) -> None:
        seeds = pathlib.Path("/r/tests/fuzz/corpus/x")
        command = run_fuzz.fuzz_command(self.BINARY, 600, self.WORK, seeds, "/a/x-")
        self.assertEqual(command[0], str(self.BINARY))
        self.assertIn("-max_total_time=600", command)
        self.assertIn(f"-timeout={run_fuzz.TIMEOUT_SECONDS}", command)
        self.assertIn(f"-rss_limit_mb={run_fuzz.RSS_LIMIT_MB}", command)
        self.assertIn("-artifact_prefix=/a/x-", command)
        # libFuzzer ghi input mới vào thư mục corpus đầu tiên: phải là corpus làm việc.
        self.assertEqual(command[-2:], [str(self.WORK), str(seeds)])

    def test_merge_command_targets_fresh_directory(self) -> None:
        merged = pathlib.Path("/w/x.merged")
        command = run_fuzz.merge_command(self.BINARY, merged, self.WORK)
        self.assertEqual(command[1], "-merge=1")
        self.assertEqual(command[-2:], [str(merged), str(self.WORK)])


@unittest.skipIf(os.name == "nt", "chương trình giả cần shebang và bit thực thi của POSIX")
class RunTest(FakeRepo):
    def setUp(self) -> None:
        super().setUp()
        self.build = self.root / "out" / "build" / "linux-fuzz"
        self.fuzz_dir = self.build / "tests" / "fuzz"
        self.fuzz_dir.mkdir(parents=True)
        binary = self.fuzz_dir / "fuzz_parser"
        binary.write_text(f"#!{sys.executable}\n{FAKE_FUZZER}", encoding="utf-8")
        make_executable(binary)
        self.write("tests/fuzz/corpus/parser/seed", b"seed")
        self.corpus = self.root / "out" / "fuzz" / "corpus"
        self.artifacts = self.root / "out" / "fuzz" / "artifacts"

    def run_fuzz(self, targets: list[str] | None = None) -> tuple[int, str]:
        return self.capture(lambda: run_fuzz.run(self.root, self.build, targets or [], 5,
                                                 self.corpus, self.artifacts))

    def calls(self) -> list[str]:
        return (self.fuzz_dir / "calls.txt").read_text(encoding="utf-8").splitlines()

    def test_fuzzes_then_replaces_work_corpus_with_merged_one(self) -> None:
        (self.corpus / "parser").mkdir(parents=True)
        (self.corpus / "parser" / "found").write_bytes(b"found")
        code, output = self.run_fuzz()
        self.assertEqual(code, 0)
        self.assertIn("run_fuzz: xanh.", output)
        calls = self.calls()
        self.assertEqual(len(calls), 2)
        self.assertIn("-max_total_time=5", calls[0])
        self.assertIn(f"-artifact_prefix={self.artifacts / 'parser'}-", calls[0])
        self.assertIn("-merge=1", calls[1])
        self.assertEqual(sorted(p.name for p in (self.corpus / "parser").iterdir()), ["found"])
        self.assertFalse((self.corpus / "parser.merged").exists())

    def test_crash_fails_and_keeps_corpus_untouched(self) -> None:
        (self.fuzz_dir / "exit_code").write_text("77")
        code, _ = self.run_fuzz(["parser"])
        self.assertEqual(code, 1)
        self.assertEqual(len(self.calls()), 1)
        self.assertFalse((self.corpus / "parser.merged").exists())

    def test_unknown_target_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_fuzz(["missing"])

    def test_missing_seed_corpus_fails(self) -> None:
        (self.root / "tests" / "fuzz" / "corpus" / "parser" / "seed").unlink()
        (self.root / "tests" / "fuzz" / "corpus" / "parser").rmdir()
        code, _ = self.run_fuzz()
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
