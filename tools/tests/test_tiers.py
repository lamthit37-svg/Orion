"""Bảng tầng của ARCH §3 có hai bản: tools/check_layers.py và cmake/orion_module.cmake. Test này
giữ hai bản khớp nhau, để cổng include và cổng link không bao giờ nói hai điều khác nhau."""

from __future__ import annotations

import re
import unittest

import check_layers

from tests.support import REPO_DIR


def cmake_engine_tiers() -> dict[str, int]:
    text = (REPO_DIR / "cmake" / "orion_module.cmake").read_text(encoding="utf-8")
    block = re.search(r"set\(ORION_ENGINE_TIERS(.*?)\)", text, re.S)
    assert block, "không thấy ORION_ENGINE_TIERS trong orion_module.cmake"
    pairs = re.findall(r"([a-z]+)=(\d+)", block.group(1))
    return {name: int(tier) for name, tier in pairs}


class TierTablesTest(unittest.TestCase):
    def test_cmake_and_python_tables_match(self) -> None:
        self.assertEqual(cmake_engine_tiers(), check_layers.ENGINE_TIERS)


if __name__ == "__main__":
    unittest.main()
