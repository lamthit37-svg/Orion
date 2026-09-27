#!/usr/bin/env python3
"""Sinh C++ cho protocol từ các tệp *.schema (ADR 0004; docs/formats/protocol.md).

CMake gọi script này lúc build (orion_add_protocol trong cmake/orion_module.cmake); code sinh ra nằm
trong out/ và không được commit. Schema sai thì in `tệp:dòng:cột: lỗi [schema]: ...` và trả 1.

Gọi tay (một dòng): `python3.13 tools/codegen/orion_codegen.py --namespace orion::protocol
--header out/gen/protocol.hpp --source out/gen/protocol.cpp
--include game/shared/protocol/protocol.hpp game/shared/protocol/*.schema`.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# Script chạy thẳng từ đường dẫn tệp: đưa tools/ vào sys.path để import gói codegen.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from codegen import cpp  # noqa: E402
from codegen import schema  # noqa: E402

_NAMESPACE = re.compile(r"[a-z][a-z0-9_]*(?:::[a-z][a-z0-9_]*)*")


def _write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n": cùng byte trên Windows và Linux.
    with path.open("w", encoding="utf-8", newline="\n") as out:
        out.write(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--namespace", required=True, help="namespace C++, ví dụ orion::protocol")
    parser.add_argument("--header", type=pathlib.Path, required=True, help="tệp .hpp sinh ra")
    parser.add_argument("--source", type=pathlib.Path, required=True, help="tệp .cpp sinh ra")
    parser.add_argument("--include", required=True,
                        help="đường include của header, như tệp .cpp sinh ra viết nó")
    parser.add_argument("--root", type=pathlib.Path, default=None,
                        help="gốc repo; tên tệp schema trong lỗi và chú thích tính từ đây")
    parser.add_argument("schemas", type=pathlib.Path, nargs="+", help="các tệp *.schema")
    args = parser.parse_args(argv)
    if not _NAMESPACE.fullmatch(args.namespace):
        parser.error(f"namespace '{args.namespace}' phải là các tên snake_case nối bằng ::")
    root = args.root.resolve() if args.root is not None else None
    paths = [p.resolve() for p in args.schemas]
    try:
        protocol = schema.load(paths, root)
    except schema.SchemaError as error:
        print(error.format(), file=sys.stderr)
        return 1
    shown = [p.relative_to(root).as_posix() if root is not None else p.as_posix() for p in paths]
    _write(args.header, cpp.generate_header(protocol, args.namespace, shown))
    _write(args.source, cpp.generate_source(protocol, args.namespace, args.include, shown))
    return 0


if __name__ == "__main__":
    sys.exit(main())
