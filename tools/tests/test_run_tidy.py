"""Test phần chọn tệp của run_tidy; bản thân clang-tidy được CI chạy trên cả repo."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

import run_tidy

from tests.support import FakeRepo


class ProjectSourcesTest(FakeRepo):
    def test_selects_project_sources_only_once(self) -> None:
        build = self.root / "out" / "build" / "linux"
        build.mkdir(parents=True)
        entries = [
            {"directory": str(build), "file": str(self.root / "engine/core/a.cpp")},
            {"directory": str(build), "file": "../../../engine/core/a.cpp"},
            {"directory": str(build), "file": str(self.root / "tests/toolchain/features.cpp")},
            {"directory": str(build), "file": str(build / "gen/game/shared/protocol/msg.cpp")},
            {"directory": str(build), "file": str(self.root / "engine/core/a.hpp")},
            {"directory": str(build), "file": "/usr/include/other.cpp"},
        ]
        database = build / "compile_commands.json"
        database.write_text(json.dumps(entries), encoding="utf-8")
        self.assertEqual(
            run_tidy.project_sources(self.root, database),
            ["engine/core/a.cpp", "tests/toolchain/features.cpp"],
        )


class HeaderFilterTest(FakeRepo):
    def test_reads_regex_from_config(self) -> None:
        (self.root / ".clang-tidy").write_bytes(
            b"Checks: '-*'\nHeaderFilterRegex: '.*/(engine|game)/.*'\nSystemHeaders: false\n")
        self.assertEqual(run_tidy.header_filter(self.root), ".*/(engine|game)/.*")

    def test_missing_regex_is_an_error(self) -> None:
        (self.root / ".clang-tidy").write_bytes(b"Checks: '-*'\n")
        with self.assertRaises(SystemExit):
            run_tidy.header_filter(self.root)

    def test_repository_config_has_a_regex(self) -> None:
        root = Path(__file__).resolve().parents[2]
        self.assertIn("engine", run_tidy.header_filter(root))


if __name__ == "__main__":
    unittest.main()
