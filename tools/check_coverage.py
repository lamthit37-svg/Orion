#!/usr/bin/env python3
"""Cổng coverage dòng của Orion (CLAUDE.md X.4), đo bằng llvm-cov trên preset linux-coverage.

Ngưỡng tối thiểu:
- 90%: game/shared, engine/net, engine/io, và code protocol sinh ra;
- 80%: phần còn lại của T0 và T1 (ARCH §3).
Module khác chỉ được báo số, chưa có ngưỡng.

Cách dùng (CI): build preset linux-coverage, chạy ctest (test preset đặt LLVM_PROFILE_FILE vào
<build>/coverage/), rồi `python3.13 tools/check_coverage.py out/build/linux-coverage`.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import shutil
import subprocess
import sys

import gatelib

T0_T1 = ("core", "math", "jobs", "crypto", "io", "net", "asset", "physics", "anim", "nav",
         "script")
HIGH_BAR = 90.0
BASE_BAR = 80.0
GENERATED_PROTOCOL = "gen/game/shared/protocol/"


@dataclasses.dataclass
class Lines:
    covered: int = 0
    count: int = 0

    @property
    def percent(self) -> float:
        return 100.0 if self.count == 0 else 100.0 * self.covered / self.count


def module_of(path: str, root: str, build: str) -> str | None:
    """Module của một tệp nguồn trong báo cáo llvm-cov; None nếu tệp không được tính (test, tệp
    ngoài repo)."""
    normalized = path.replace("\\", "/")
    if normalized.startswith(build):
        rel = normalized[len(build):].lstrip("/")
        return "protocol (sinh ra)" if rel.startswith(GENERATED_PROTOCOL) else None
    if not normalized.startswith(root):
        return None
    rel = normalized[len(root):].lstrip("/")
    parts = rel.split("/")
    if "tests" in parts[:-1]:
        return None
    if parts[0] == "engine" and len(parts) >= 3:
        return f"engine/{parts[1]}"
    if parts[0] == "game" and len(parts) >= 3:
        if parts[1] == "server" and parts[2] == "lib" and len(parts) >= 5:
            return f"game/server/lib/{parts[3]}"
        if parts[1] == "server" and len(parts) >= 4:
            return f"game/server/{parts[2]}"
        return f"game/{parts[1]}"
    return None


def threshold(module: str) -> float | None:
    if module in ("game/shared", "engine/net", "engine/io", "protocol (sinh ra)"):
        return HIGH_BAR
    if module.startswith("engine/") and module.split("/", 1)[1] in T0_T1:
        return BASE_BAR
    return None


def aggregate(report: dict, root: str, build: str) -> dict[str, Lines]:
    modules: dict[str, Lines] = {}
    for export in report.get("data", []):
        for entry in export.get("files", []):
            module = module_of(entry["filename"], root, build)
            if module is None:
                continue
            lines = entry["summary"]["lines"]
            total = modules.setdefault(module, Lines())
            total.covered += int(lines["covered"])
            total.count += int(lines["count"])
    return modules


def evaluate(modules: dict[str, Lines]) -> tuple[list[str], list[str]]:
    """(dòng báo cáo, lỗi) theo thứ tự tên module."""
    rows: list[str] = []
    failures: list[str] = []
    for module in sorted(modules):
        lines = modules[module]
        bar = threshold(module)
        verdict = "chưa có ngưỡng" if bar is None else f"ngưỡng {bar:.0f}%"
        rows.append(f"{module:<28} {lines.percent:6.2f}%  ({lines.covered}/{lines.count})  "
                    f"{verdict}")
        if bar is not None and lines.percent < bar:
            failures.append(f"{module}: {lines.percent:.2f}% < {bar:.0f}%")
    return rows, failures


def find_tool(name: str) -> str:
    for candidate in (f"{name}-20", name):
        found = shutil.which(candidate)
        if found:
            return found
    raise SystemExit(f"check_coverage: không thấy {name}; cài gói llvm-20")


def test_binaries(build: pathlib.Path) -> list[str]:
    """Tệp chạy test và benchmark do orion_add_module tạo (tên *_tests, *_bench) trong cây build.

    Không lấy từ `ctest --show-only`: gtest_discover_tests ở chế độ PRE_TEST gọi test qua cmake -P,
    nên lệnh của test không phải tệp chạy thật.
    """
    found: list[str] = []
    for dirpath, dirnames, filenames in os.walk(build):
        dirnames[:] = [d for d in dirnames if d not in ("vcpkg_installed", "CMakeFiles")]
        for name in filenames:
            path = pathlib.Path(dirpath, name)
            if (name.endswith("_tests") or name.endswith("_bench")) and os.access(path, os.X_OK):
                found.append(str(path))
    return sorted(found)


def fresh_profiles(raw_dir: pathlib.Path, binaries: list[str]) -> tuple[list[str], int]:
    """Tệp .profraw ghi từ lần build gần nhất trở đi, và số tệp cũ hơn bị bỏ qua.

    Mỗi tiến trình test ghi một tệp riêng (%p-%m), nên thư mục còn giữ tệp của các lần build trước.
    Hàm trong tệp cũ có hash cấu trúc khác tệp chạy hiện tại: llvm-cov bỏ các hàm đó ("mismatched
    data") và số dòng sai đi. Tệp chạy test mới nhất đánh dấu lần build gần nhất.
    """
    built = max(pathlib.Path(binary).stat().st_mtime_ns for binary in binaries)
    fresh: list[str] = []
    stale = 0
    for path in sorted(raw_dir.glob("*.profraw")):
        if path.stat().st_mtime_ns >= built:
            fresh.append(str(path))
        else:
            stale += 1
    return fresh, stale


def collect(build: pathlib.Path) -> dict:
    binaries = test_binaries(build)
    if not binaries:
        raise SystemExit("check_coverage: ctest không có tệp chạy test nào")
    raw_dir = build / "coverage"
    raw, stale = fresh_profiles(raw_dir, binaries)
    if stale:
        print(f"check_coverage: bỏ qua {stale} tệp .profraw cũ hơn lần build gần nhất")
    if not raw:
        raise SystemExit(f"check_coverage: không có tệp .profraw nào mới hơn lần build gần nhất "
                         f"trong {raw_dir}; chạy ctest trước")
    merged = raw_dir / "coverage.profdata"
    subprocess.run([find_tool("llvm-profdata"), "merge", "-sparse", "-o", str(merged), *raw],
                   check=True)
    objects = [arg for binary in binaries[1:] for arg in ("-object", binary)]
    export = subprocess.run(
        [find_tool("llvm-cov"), "export", "-summary-only", f"-instr-profile={merged}",
         binaries[0], *objects],
        capture_output=True, text=True, check=True,
    ).stdout
    return json.loads(export)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("build_dir", type=pathlib.Path, help="thư mục build của linux-coverage")
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    args = parser.parse_args(argv)
    root = (args.root.resolve() if args.root else gatelib.repo_root()).as_posix()
    build = args.build_dir.resolve()
    modules = aggregate(collect(build), root, build.as_posix())
    rows, failures = evaluate(modules)
    print("\n".join(rows))
    for failure in failures:
        print(f"check_coverage: lỗi [coverage/line]: {failure} (X.4)")
    if failures:
        print(f"check_coverage: {len(failures)} module dưới ngưỡng.", file=sys.stderr)
        return 1
    print("check_coverage: xanh.")
    return 0


if __name__ == "__main__":
    os.environ.setdefault("LC_ALL", "C.UTF-8")
    sys.exit(main())
