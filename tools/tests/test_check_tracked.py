"""Test cổng tệp nguồn bị .gitignore nuốt."""

from __future__ import annotations

import unittest

import check_tracked

from tests.support import FakeRepo


class TrackedTest(FakeRepo):
    def run_gate(self) -> tuple[int, list[tuple[str, str]]]:
        code, output = self.capture(lambda: check_tracked.main(["--root", str(self.root)]))
        return code, self.rules(output)

    def test_ignored_source_is_reported(self) -> None:
        self.write(".gitignore", "/out/\n*generated*\n")
        self.write("engine/core/generated_table.cpp", "int x;\n")
        self.write("engine/core/a.cpp", "int a;\n")
        self.assertEqual(
            self.run_gate(), (1, [("engine/core/generated_table.cpp", "tracked/ignored")])
        )

    def test_output_and_editor_directories_are_not_sources(self) -> None:
        self.write(".gitignore", "/out/\n/.vs/\n")
        self.write("out/build/dev/gen/a.cpp", "int a;\n")
        self.write(".vs/settings.json", "{}\n")
        self.assertEqual(self.run_gate(), (0, []))

    def test_force_added_file_is_not_reported(self) -> None:
        self.write(".gitignore", "*.json\n")
        self.write("CMakePresets.json", "{}\n")
        self.git("add", "-f", "CMakePresets.json")
        self.assertEqual(self.run_gate(), (0, []))

    def test_github_directory_is_scanned(self) -> None:
        self.write(".gitignore", "*.yml\n")
        self.write(".github/workflows/ci.yml", "on: push\n")
        self.assertEqual(self.run_gate(), (1, [(".github/workflows/ci.yml", "tracked/ignored")]))


if __name__ == "__main__":
    unittest.main()
