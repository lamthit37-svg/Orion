"""Test phần dùng chung của cổng kiểm."""

from __future__ import annotations

import unittest

import gatelib

from tests.support import FakeRepo


class StripCppTest(unittest.TestCase):
    def assert_same_shape(self, source: str, stripped: str) -> None:
        self.assertEqual(len(source), len(stripped))
        self.assertEqual(source.count("\n"), stripped.count("\n"))

    def test_removes_comments_keeps_lines(self) -> None:
        source = "int a; // _WIN32\n/* __linux__\n ok */ int b;\n"
        stripped = gatelib.strip_cpp(source, keep_strings=True)
        self.assert_same_shape(source, stripped)
        self.assertNotIn("_WIN32", stripped)
        self.assertNotIn("__linux__", stripped)
        self.assertIn("int b;", stripped)

    def test_line_comment_continuation(self) -> None:
        source = "// nối \\\n_WIN32\nint c;\n"
        stripped = gatelib.strip_cpp(source, keep_strings=False)
        self.assertNotIn("_WIN32", stripped)
        self.assertIn("int c;", stripped)

    def test_strings_kept_or_blanked(self) -> None:
        source = 'const char* s = "_WIN32 // not a comment";\n'
        kept = gatelib.strip_cpp(source, keep_strings=True)
        blanked = gatelib.strip_cpp(source, keep_strings=False)
        self.assertEqual(source, kept)
        self.assert_same_shape(source, blanked)
        self.assertNotIn("_WIN32", blanked)

    def test_raw_string_with_delimiter_and_prefix(self) -> None:
        source = 'auto s = u8R"x(say "hi" )" // _WIN32 )x";\nint d;\n'
        blanked = gatelib.strip_cpp(source, keep_strings=False)
        self.assert_same_shape(source, blanked)
        self.assertNotIn("_WIN32", blanked)
        self.assertIn("int d;", blanked)

    def test_char_literals_and_digit_separators(self) -> None:
        source = "auto n = 1'000'000; auto c = L'\\''; auto q = '\"'; int e;\n"
        blanked = gatelib.strip_cpp(source, keep_strings=False)
        self.assert_same_shape(source, blanked)
        self.assertIn("1'000'000", blanked)
        self.assertIn("int e;", blanked)

    def test_unterminated_string_stops_at_end_of_line(self) -> None:
        source = 'auto s = "broken\nint f;\n'
        blanked = gatelib.strip_cpp(source, keep_strings=False)
        self.assert_same_shape(source, blanked)
        self.assertIn("int f;", blanked)


class ListFilesTest(FakeRepo):
    def test_lists_tracked_and_new_but_not_ignored(self) -> None:
        self.write(".gitignore", "/out/\nsecret.txt\n")
        self.write("engine/core/a.cpp", "int a;\n")
        self.write("out/build/x.cpp", "int x;\n")
        self.write("secret.txt", "x\n")
        files = gatelib.list_source_files(self.root)
        self.assertIn("engine/core/a.cpp", files)
        self.assertNotIn("out/build/x.cpp", files)
        self.assertNotIn("secret.txt", files)

    def test_select_filters_by_file_or_directory(self) -> None:
        paths = ["engine/core/a.cpp", "engine/core2/b.cpp", "game/shared/c.cpp"]
        self.assertEqual(gatelib.select(paths, ["engine/core"]), ["engine/core/a.cpp"])
        self.assertEqual(gatelib.select(paths, ["game/shared/c.cpp"]), ["game/shared/c.cpp"])
        self.assertEqual(gatelib.select(paths, None), paths)


if __name__ == "__main__":
    unittest.main()
