"""Test bộ sinh dữ liệu tham chiếu của engine/math: giá trị đã biết, và tệp đã commit còn khớp."""

from __future__ import annotations

import contextlib
import io
import math
import unittest
from decimal import Decimal, localcontext

import gen_math_reference as gen


def at_precision(function, *args: float) -> float:
    with localcontext() as context:
        context.prec = gen.PRECISION
        return float(function(*(Decimal(a) for a in args)))


class KnownValuesTest(unittest.TestCase):
    def test_matches_correctly_rounded_constants(self) -> None:
        # Giá trị làm tròn đúng đã biết, độc lập với cách tính của script.
        self.assertEqual(at_precision(gen.d_sin, 1.0), 0.8414709848078965)
        self.assertEqual(at_precision(gen.d_cos, 1.0), 0.5403023058681398)
        self.assertEqual(at_precision(gen.d_atan, 1.0), math.pi / 4)
        self.assertEqual(at_precision(gen.d_atan2, -1.0, -1.0), -3 * math.pi / 4)
        self.assertEqual(at_precision(gen.d_atan, 1e300), math.pi / 2)
        self.assertEqual(at_precision(gen.d_sin, math.pi), 1.2246467991473532e-16)

    def test_tiny_arguments_keep_their_value(self) -> None:
        self.assertEqual(at_precision(gen.d_atan, 1e-300), 1e-300)
        self.assertEqual(at_precision(gen.d_atan2, 2.0 ** -900, 3.0), 2.0 ** -900 / 3.0)
        self.assertEqual(at_precision(gen.d_sin, -1e-200), -1e-200)

    def test_bits_round_trip(self) -> None:
        self.assertEqual(gen.bits(1.0), "3ff0000000000000")
        self.assertEqual(gen.from_bits("bff0000000000000"), -1.0)
        self.assertEqual(gen.nudge(1.0, 1), 1.0000000000000002)
        self.assertEqual(gen.nudge(1.0, -1), 0.9999999999999999)


class CommittedFileTest(unittest.TestCase):
    def test_committed_file_matches_generator(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gen.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main()
