#!/usr/bin/env python3
"""Chạy các fuzz target libFuzzer của Orion (CLAUDE.md X.4, X.16.9), cùng một cách trên máy dev và
trong job fuzz đêm của CI (ARCH §8 mục 6).

Mỗi target fuzz_<tên> trong <build>/tests/fuzz chạy --seconds giây (mặc định 600, vì X.16.9 đòi ít
nhất 10 phút khi chạm parser) trên corpus làm việc <corpus>/<tên>, cộng corpus hạt giống
tests/fuzz/corpus/<tên> chỉ để đọc. Chạy xong, corpus làm việc được thu gọn bằng -merge=1 để nó
không phình qua các đêm. Crash, timeout hay vượt bộ nhớ làm script trả mã 1; input gây lỗi nằm ở
<artifacts>/<tên>-*, và libFuzzer in thêm dạng base64 của nó vào log.

Gọi tay: `python3.13 tools/run_fuzz.py -p out/build/linux-fuzz --target <tên>` sau khi build preset
linux-fuzz.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

import gatelib

PREFIX = "fuzz_"
DEFAULT_SECONDS = 600
# Parser là hàm toàn phần (X.9): một input chạy quá vài giây đã là lỗi đáng báo.
TIMEOUT_SECONDS = 10
RSS_LIMIT_MB = 2048


def discover(build_dir: pathlib.Path) -> list[str]:
    """Tên (bỏ tiền tố fuzz_) của mọi target fuzz đã build, theo thứ tự tên."""
    fuzz_dir = build_dir / "tests" / "fuzz"
    if not fuzz_dir.is_dir():
        return []
    return sorted(
        path.name[len(PREFIX):]
        for path in fuzz_dir.iterdir()
        if path.name.startswith(PREFIX) and path.is_file() and os.access(path, os.X_OK)
    )


def fuzz_command(binary: pathlib.Path, seconds: int, work: pathlib.Path, seeds: pathlib.Path,
                 artifact_prefix: str) -> list[str]:
    """Lệnh fuzz: libFuzzer chỉ ghi input mới vào thư mục corpus đầu tiên, tức corpus làm việc."""
    return [
        str(binary),
        f"-max_total_time={seconds}",
        f"-timeout={TIMEOUT_SECONDS}",
        f"-rss_limit_mb={RSS_LIMIT_MB}",
        "-print_final_stats=1",
        f"-artifact_prefix={artifact_prefix}",
        str(work),
        str(seeds),
    ]


def merge_command(binary: pathlib.Path, merged: pathlib.Path, work: pathlib.Path) -> list[str]:
    """Lệnh thu gọn: chép sang `merged` tập input nhỏ nhất giữ nguyên độ phủ của `work`."""
    return [
        str(binary),
        "-merge=1",
        f"-timeout={TIMEOUT_SECONDS}",
        f"-rss_limit_mb={RSS_LIMIT_MB}",
        str(merged),
        str(work),
    ]


def fuzz_one(root: pathlib.Path, build_dir: pathlib.Path, name: str, seconds: int,
             corpus_dir: pathlib.Path, artifact_dir: pathlib.Path) -> bool:
    binary = build_dir / "tests" / "fuzz" / f"{PREFIX}{name}"
    seeds = root / "tests" / "fuzz" / "corpus" / name
    if not seeds.is_dir():
        print(f"run_fuzz: {name}: thiếu corpus hạt giống {seeds}", file=sys.stderr)
        return False
    work = corpus_dir / name
    work.mkdir(parents=True, exist_ok=True)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    print(f"run_fuzz: {name}: fuzz {seconds} giây", flush=True)
    command = fuzz_command(binary, seconds, work, seeds, f"{artifact_dir / name}-")
    code = subprocess.run(command, check=False).returncode
    if code != 0:
        print(f"run_fuzz: {name}: libFuzzer thoát với mã {code}; xem {artifact_dir}",
              file=sys.stderr)
        return False
    merged = corpus_dir / f"{name}.merged"
    shutil.rmtree(merged, ignore_errors=True)
    merged.mkdir()
    code = subprocess.run(merge_command(binary, merged, work), check=False).returncode
    if code != 0:
        print(f"run_fuzz: {name}: thu gọn corpus thoát với mã {code}", file=sys.stderr)
        shutil.rmtree(merged, ignore_errors=True)
        return False
    shutil.rmtree(work)
    merged.rename(work)
    print(f"run_fuzz: {name}: xanh, corpus còn {sum(1 for _ in work.iterdir())} input",
          flush=True)
    return True


def run(root: pathlib.Path, build_dir: pathlib.Path, targets: list[str], seconds: int,
        corpus_dir: pathlib.Path, artifact_dir: pathlib.Path) -> int:
    built = discover(build_dir)
    if not built:
        raise SystemExit(f"run_fuzz: không thấy target {PREFIX}* trong {build_dir}/tests/fuzz; "
                         "build preset linux-fuzz trước")
    unknown = sorted(set(targets) - set(built))
    if unknown:
        raise SystemExit(f"run_fuzz: không có target {', '.join(unknown)}; "
                         f"đã build: {', '.join(built)}")
    failed = [
        name
        for name in (targets or built)
        if not fuzz_one(root, build_dir, name, seconds, corpus_dir, artifact_dir)
    ]
    if failed:
        print(f"run_fuzz: đỏ: {', '.join(failed)}", file=sys.stderr)
        return 1
    print("run_fuzz: xanh.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("-p", "--build-dir", type=pathlib.Path, required=True,
                        help="thư mục build của preset linux-fuzz")
    parser.add_argument("--target", action="append", default=[], dest="targets",
                        help="chỉ chạy target này (bỏ tiền tố fuzz_); lặp lại được")
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS,
                        help=f"thời gian cho mỗi target, mặc định {DEFAULT_SECONDS}")
    parser.add_argument("--corpus-dir", type=pathlib.Path, default=pathlib.Path("out/fuzz/corpus"),
                        help="corpus làm việc, mỗi target một thư mục con")
    parser.add_argument("--artifact-dir", type=pathlib.Path,
                        default=pathlib.Path("out/fuzz/artifacts"),
                        help="nơi libFuzzer ghi input gây lỗi")
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    args = parser.parse_args(argv)
    if args.seconds < 1:
        parser.error("--seconds phải dương")
    root = args.root.resolve() if args.root else gatelib.repo_root()
    return run(root, args.build_dir.resolve(), args.targets, args.seconds,
               (root / args.corpus_dir).resolve(), (root / args.artifact_dir).resolve())


if __name__ == "__main__":
    sys.exit(main())
