"""Test cổng style: mỗi luật có một ca đỏ và một ca xanh."""

from __future__ import annotations

import unittest
from unittest import mock

import check_style

from tests.support import FakeRepo

DEBT = "TO" + "DO"
HEDGE = "có" + " lẽ"


def rules_of(path: str, content: str | bytes) -> list[str]:
    raw = content if isinstance(content, bytes) else content.encode("utf-8")
    return [f.rule for f in check_style.check_text(path, raw)]


class TextRulesTest(unittest.TestCase):
    def test_clean_file_passes(self) -> None:
        self.assertEqual(rules_of("engine/core/a.cpp", "int a;\n"), [])
        self.assertEqual(rules_of("docs/x.md", ""), [])

    def test_bom_and_invalid_utf8(self) -> None:
        self.assertIn("encoding/bom", rules_of("a.cpp", b"\xef\xbb\xbfint a;\n"))
        self.assertIn("encoding/utf8", rules_of("a.cpp", b"int a; \xff\n"))

    def test_line_endings(self) -> None:
        self.assertIn("whitespace/eol", rules_of("a.cpp", "int a;\r\n"))
        self.assertIn("whitespace/eol", rules_of("dev.bat", "echo 1\n"))
        self.assertEqual(rules_of("dev.bat", "echo 1\r\n"), [])

    def test_tabs_and_trailing_spaces(self) -> None:
        self.assertIn("whitespace/tab", rules_of("a.cpp", "\tint a;\n"))
        self.assertIn("whitespace/trailing", rules_of("a.md", "chữ \n"))

    def test_end_of_file(self) -> None:
        self.assertIn("whitespace/eof", rules_of("a.md", "không xuống dòng"))
        self.assertIn("whitespace/eof", rules_of("a.md", "thừa\n\n"))

    def test_line_length_only_for_code(self) -> None:
        long_line = "x" * 101 + "\n"
        self.assertIn("length/line", rules_of("tools/a.py", long_line))
        self.assertIn("length/line", rules_of("engine/CMakeLists.txt", long_line))
        self.assertEqual(rules_of("docs/a.md", long_line), [])
        self.assertEqual(rules_of("tools/a.py", "x" * 100 + "\n"), [])

    def test_line_length_counts_characters_not_bytes(self) -> None:
        self.assertEqual(rules_of("tools/a.py", "ệ" * 100 + "\n"), [])

    def test_debt_marker_needs_code(self) -> None:
        self.assertIn("comment/debt", rules_of("a.cpp", f"// {DEBT}: sửa sau\n"))
        self.assertEqual(rules_of("a.cpp", f"// {DEBT}(NGHI-NGO-012): chờ đo\n"), [])
        self.assertEqual(rules_of("docs/a.md", f"{DEBT} trong tài liệu\n"), [])

    def test_hedge_words_in_code_only(self) -> None:
        self.assertIn("comment/hedge", rules_of("a.cpp", f"// {HEDGE} nhanh hơn\n"))
        self.assertIn("comment/hedge", rules_of("a.py", f"# {HEDGE.upper()}\n"))
        self.assertEqual(rules_of("docs/a.md", f"{HEDGE}\n"), [])

    def test_header_rules(self) -> None:
        self.assertIn("header/pragma-once", rules_of("engine/core/a.hpp", "int a;\n"))
        self.assertEqual(rules_of("engine/core/a.hpp", "#pragma once\nint a;\n"), [])
        self.assertIn(
            "header/using-namespace",
            rules_of("engine/core/a.hpp", "#pragma once\nusing namespace std;\n"),
        )
        self.assertEqual(
            rules_of("engine/core/a.hpp", "#pragma once\n// using namespace std;\n"), []
        )
        self.assertEqual(rules_of("engine/core/a.cpp", "using namespace std;\n"), [])

    def test_cpp_file_length(self) -> None:
        self.assertIn("length/file", rules_of("a.cpp", "int a;\n" * 1001))
        self.assertEqual(rules_of("a.cpp", "int a;\n" * 1000), [])


class RunTest(FakeRepo):
    def setUp(self) -> None:
        super().setUp()
        self.copy_from_repo(".clang-format")

    def run_gate(self, *extra: str) -> tuple[int, str]:
        args = ["--root", str(self.root), *extra]
        return self.capture(lambda: check_style.main(args))

    def test_formatted_code_passes_and_unformatted_fails(self) -> None:
        self.write("engine/core/a.cpp", "int f(int x) {\n    return x;\n}\n")
        code, output = self.run_gate()
        self.assertEqual(code, 0, output)
        self.write("engine/core/b.cpp", "int g(int x){return x;}\n")
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertEqual(self.rules(output), [("engine/core/b.cpp", "format/clang-format")])

    def test_fix_repairs_format_and_whitespace(self) -> None:
        self.write("engine/core/b.cpp", "int g(int x){return x;}   \n\n\n")
        self.write("docs/a.md", "chữ  \r\n")
        code, output = self.run_gate("--fix")
        self.assertEqual(code, 0, output)
        self.assertEqual(
            (self.root / "engine/core/b.cpp").read_text(encoding="utf-8"),
            "int g(int x) {\n    return x;\n}\n",
        )
        self.assertEqual((self.root / "docs/a.md").read_bytes(), "chữ\n".encode())

    def test_excluded_and_binary_files_are_skipped(self) -> None:
        self.write("third_party/lib/x.cpp", "int   x ;\t\n")
        self.write("content/t.png", b"\x89PNG\r\n\x00\x00")
        code, output = self.run_gate()
        self.assertEqual(code, 0, output)

    def test_cache_skips_unchanged_files(self) -> None:
        self.write(".gitignore", "/out/\n")
        self.write("engine/core/a.cpp", "int f(int x) {\n    return x;\n}\n")
        cache = self.root / "out" / "style.json"
        self.assertEqual(self.run_gate("--cache", str(cache))[0], 0)
        with mock.patch.object(check_style, "check_format", side_effect=AssertionError):
            self.assertEqual(self.run_gate("--cache", str(cache))[0], 0)
        self.write("engine/core/a.cpp", "int f(int x){return x;}\n")
        self.assertEqual(self.run_gate("--cache", str(cache))[0], 1)

    def test_paths_argument_limits_scope(self) -> None:
        self.write("engine/core/a.cpp", "int f(int x){return x;}\n")
        self.write("game/shared/b.cpp", "int g(int x) {\n    return x;\n}\n")
        self.assertEqual(self.run_gate("game/shared")[0], 0)
        self.assertEqual(self.run_gate("engine")[0], 1)


if __name__ == "__main__":
    unittest.main()
