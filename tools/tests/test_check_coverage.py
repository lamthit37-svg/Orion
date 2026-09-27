"""Test phần gom và so ngưỡng của cổng coverage; llvm-cov do job CI chạy thật."""

from __future__ import annotations

import unittest

import check_coverage

ROOT = "/repo"
BUILD = "/repo/out/build/linux-coverage"


def entry(path: str, covered: int, count: int) -> dict:
    return {"filename": path, "summary": {"lines": {"covered": covered, "count": count}}}


class ModuleOfTest(unittest.TestCase):
    def test_maps_sources_to_modules(self) -> None:
        self.assertEqual(check_coverage.module_of("/repo/engine/core/log.cpp", ROOT, BUILD),
                         "engine/core")
        self.assertEqual(check_coverage.module_of("/repo/game/shared/movement/move.cpp", ROOT,
                                                  BUILD), "game/shared")
        self.assertEqual(check_coverage.module_of("/repo/game/server/lib/db/pg.cpp", ROOT, BUILD),
                         "game/server/lib/db")
        self.assertEqual(check_coverage.module_of("/repo/game/server/auth/login.cpp", ROOT, BUILD),
                         "game/server/auth")
        self.assertEqual(
            check_coverage.module_of(BUILD + "/gen/game/shared/protocol/messages.cpp", ROOT, BUILD),
            "protocol (sinh ra)")

    def test_ignores_tests_and_outside_files(self) -> None:
        self.assertIsNone(check_coverage.module_of("/repo/engine/core/tests/log_test.cpp", ROOT,
                                                   BUILD))
        self.assertIsNone(check_coverage.module_of("/usr/include/c++/14/vector", ROOT, BUILD))
        self.assertIsNone(check_coverage.module_of(BUILD + "/vcpkg_installed/x.h", ROOT, BUILD))


class EvaluateTest(unittest.TestCase):
    def test_thresholds_follow_claude_md(self) -> None:
        report = {"data": [{"files": [
            entry("/repo/engine/core/a.cpp", 80, 100),
            entry("/repo/engine/core/b.hpp", 2, 2),
            entry("/repo/engine/net/c.cpp", 89, 100),
            entry("/repo/engine/render/d.cpp", 1, 100),
            entry("/repo/engine/io/e.cpp", 90, 100),
        ]}]}
        modules = check_coverage.aggregate(report, ROOT, BUILD)
        self.assertEqual(modules["engine/core"].covered, 82)
        self.assertEqual(modules["engine/core"].count, 102)
        _, failures = check_coverage.evaluate(modules)
        self.assertEqual(failures, ["engine/net: 89.00% < 90%"])

    def test_module_without_lines_counts_as_covered(self) -> None:
        self.assertEqual(check_coverage.Lines().percent, 100.0)


if __name__ == "__main__":
    unittest.main()
