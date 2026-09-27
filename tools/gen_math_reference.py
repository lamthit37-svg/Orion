"""Sinh dữ liệu tham chiếu làm tròn đúng cho test độ chính xác của engine/math.

Mỗi dòng của tệp ra là một ca: tên hàm, bit của đầu vào và bit của kết quả đúng đã làm tròn về f64
gần nhất, dạng hex 16 chữ số:

    sin 3ff0000000000000 3feaed548f090cee
    atan2 <y> <x> <kết quả>

Giá trị đúng được tính bằng decimal của thư viện chuẩn ở 80 chữ số (khoảng 265 bit), rồi đổi về
f64 bằng float(), vốn làm tròn đúng. Đầu vào sinh từ một seed cố định nên tệp tái tạo được từng
byte; --check so tệp đã commit với bản sinh lại.

Chạy: python tools/gen_math_reference.py [--check]
"""

from __future__ import annotations

import argparse
import random
import struct
import sys
from decimal import Decimal, localcontext
from pathlib import Path

OUTPUT = Path(__file__).resolve().parent.parent / "engine/math/tests/data/trig_reference.txt"
SEED = 20260927
PRECISION = 80

# 110 chữ số của π, đủ để giảm đối số tới 2^20 · π/2 mà vẫn giữ 80 chữ số.
PI = Decimal("3.14159265358979323846264338327950288419716939937510582097494459230781640628620899"
             "862803482534211706798214808651")
with localcontext() as _context:
    _context.prec = 120  # chia đôi 110 chữ số là chính xác ở độ chính xác này
    HALF_PI = PI / 2
# 2^20 · π/2 làm tròn về f64: biên của vùng giảm đối số chính xác trong trig.hpp.
REDUCTION_RANGE = 1647099.3291652855


def taylor(x: Decimal, first_power: int) -> Decimal:
    """Tổng của (-1)^n x^(2n+p) / (2n+p)! với p = first_power (1 cho sin, 0 cho cos)."""
    term = Decimal(1)
    for k in range(1, first_power + 1):
        term = term * x / k
    total = term
    x2 = x * x
    power = first_power
    epsilon = Decimal(10) ** -(PRECISION + 5)
    while abs(term) > epsilon:
        term = -term * x2 / ((power + 1) * (power + 2))
        power += 2
        total += term
    return total


def reduce(x: Decimal) -> tuple[Decimal, int]:
    """x = k · π/2 + r với |r| <= π/4; trả (r, k mod 4)."""
    k = (x / HALF_PI).to_integral_value()
    return x - k * HALF_PI, int(k) % 4


def d_sin(x: Decimal) -> Decimal:
    r, quadrant = reduce(x)
    value = taylor(r, 1) if quadrant % 2 == 0 else taylor(r, 0)
    return value if quadrant < 2 else -value


def d_cos(x: Decimal) -> Decimal:
    r, quadrant = reduce(x)
    value = taylor(r, 0) if quadrant % 2 == 0 else taylor(r, 1)
    return value if quadrant in (0, 3) else -value


def d_atan(x: Decimal) -> Decimal:
    """atan bằng công thức nửa góc atan(x) = 2 atan(x / (1 + sqrt(1 + x²))) rồi Taylor."""
    if x < 0:
        return -d_atan(-x)
    if x > 1:
        return HALF_PI - d_atan(1 / x)
    doublings = 0
    while x > Decimal("0.01"):
        x = x / (1 + (1 + x * x).sqrt())
        doublings += 1
    # Sai số dừng tính tương đối với tổng: x có thể nhỏ tới 1e-300.
    total = x
    power = x
    x2 = x * x
    n = 0
    tolerance = abs(x) * Decimal(10) ** -(PRECISION + 5)
    while True:
        n += 1
        power *= x2
        term = power / (2 * n + 1)
        if term <= tolerance:
            break
        total += -term if n % 2 == 1 else term
    return total * (2 ** doublings)


def d_atan2(y: Decimal, x: Decimal) -> Decimal:
    if x > 0:
        return d_atan(y / x)
    if x < 0:
        return d_atan(y / x) + (PI if y >= 0 else -PI)
    return HALF_PI if y > 0 else -HALF_PI


def bits(value: float) -> str:
    return struct.pack(">d", value).hex()


def from_bits(text: str) -> float:
    return struct.unpack(">d", bytes.fromhex(text))[0]


def exact(value: float) -> Decimal:
    return Decimal(value)  # chuyển f64 sang decimal là chính xác


def nudge(value: float, ulps: int) -> float:
    """Dịch value đi ulps giá trị f64 kề nhau (cùng dấu, không qua 0)."""
    raw = struct.unpack(">q", struct.pack(">d", value))[0]
    return struct.unpack(">d", struct.pack(">q", raw + ulps))[0]


def log_uniform(rng: random.Random, low_exp: int, high_exp: int) -> float:
    return rng.choice((-1.0, 1.0)) * 2.0 ** rng.uniform(low_exp, high_exp)


def trig_inputs(rng: random.Random) -> list[float]:
    values = [0.5, 1.0, 2.0, 3.0, -10.0, 2.0 ** -27, -(2.0 ** -26), REDUCTION_RANGE,
              -REDUCTION_RANGE, 1e5, float(HALF_PI), float(PI)]
    values += [rng.uniform(-0.785, 0.785) for _ in range(64)]
    values += [rng.uniform(-25.2, 25.2) for _ in range(64)]
    values += [rng.uniform(-REDUCTION_RANGE, REDUCTION_RANGE) for _ in range(64)]
    for _ in range(64):
        # Gần bội của π/2: nơi phần dư sau giảm đối số nhỏ nhất.
        k = rng.randint(1, 2 ** 20)
        values.append(nudge(float(k * HALF_PI), rng.randint(-8, 8)))
    return values


def atan_inputs(rng: random.Random) -> list[float]:
    values = [1.0, -1.0, 0.25, 0.3125, 0.4375, 0.5625, 0.6875, 0.8125, 0.9375, 2.0 ** 27,
              nudge(2.0 ** 27, -1), 1e300, -1e-9]
    values += [rng.uniform(-1.0, 1.0) for _ in range(96)]
    values += [log_uniform(rng, -30, 30) for _ in range(96)]
    return values


def atan2_inputs(rng: random.Random) -> list[tuple[float, float]]:
    pairs = [(1.0, 1.0), (-1.0, -1.0), (1.0, -3.0), (-2.0, 0.5), (1e-300, -1.0),
             (3.0 * 2.0 ** 600, 2.0 ** 601), (2.0 ** -900, 3.0 * 2.0 ** -899),
             (2.0 ** 1000, -(2.0 ** 1001))]
    pairs += [(log_uniform(rng, -20, 20), log_uniform(rng, -20, 20)) for _ in range(160)]
    pairs += [(log_uniform(rng, -1000, 1000), log_uniform(rng, -1000, 1000)) for _ in range(32)]
    return pairs


def asin_inputs(rng: random.Random) -> list[float]:
    values = [1.0, -1.0, 0.5, -0.5, nudge(1.0, -1), 2.0 ** -27, -(2.0 ** -20)]
    values += [rng.uniform(-1.0, 1.0) for _ in range(96)]
    values += [rng.choice((-1.0, 1.0)) * (1.0 - 2.0 ** rng.uniform(-52, -1)) for _ in range(64)]
    values += [log_uniform(rng, -40, -2) for _ in range(32)]
    return values


def generate() -> str:
    rng = random.Random(SEED)
    lines: list[str] = []
    with localcontext() as context:
        context.prec = PRECISION
        trig = trig_inputs(rng)
        for x in trig:
            lines.append(f"sin {bits(x)} {bits(float(d_sin(exact(x))))}")
        for x in trig:
            lines.append(f"cos {bits(x)} {bits(float(d_cos(exact(x))))}")
        for x in trig:
            ratio = d_sin(exact(x)) / d_cos(exact(x))
            lines.append(f"tan {bits(x)} {bits(float(ratio))}")
        for x in atan_inputs(rng):
            lines.append(f"atan {bits(x)} {bits(float(d_atan(exact(x))))}")
        for y, x in atan2_inputs(rng):
            value = float(d_atan2(exact(y), exact(x)))
            lines.append(f"atan2 {bits(y)} {bits(x)} {bits(value)}")
        for x in asin_inputs(rng):
            root = (1 - exact(x) * exact(x)).sqrt()
            lines.append(f"asin {bits(x)} {bits(float(d_atan2(exact(x), root)))}")
            lines.append(f"acos {bits(x)} {bits(float(d_atan2(root, exact(x))))}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="so tệp đã commit với bản sinh lại, không ghi")
    args = parser.parse_args(argv)
    content = generate()
    if args.check:
        if OUTPUT.read_text(encoding="utf-8") != content:
            print(f"gen_math_reference: {OUTPUT} khác bản sinh lại; chạy lại script",
                  file=sys.stderr)
            return 1
        print("gen_math_reference: khớp.")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(content.encode("utf-8"))
    print(f"gen_math_reference: đã ghi {content.count(chr(10))} ca vào {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
