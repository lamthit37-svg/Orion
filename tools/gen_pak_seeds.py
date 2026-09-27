#!/usr/bin/env python3
"""Sinh pak hạt giống cho fuzz target io_pak_index, viết độc lập với engine/io theo đặc tả.

Bộ ghi này chỉ dựa vào docs/formats/pak.md (struct, hashlib.blake2b), không dùng code C++, nên test
PakSeeds của engine/io là phép kiểm chéo giữa đặc tả và code: PakIndex phải đọc đúng các pak này,
và từ chối hai tệp sai với đúng mã lỗi của bảng "Lỗi". Blob Zstd là frame chỉ có một khối raw, dựng
tay theo doc/zstd_compression_format.md (bản 0.4.3, trong zstd v1.5.7), nên không cần thư viện zstd.

Tám tệp có tên (empty, one_stored, ...) do script này sinh; các tệp tên hex khác trong thư mục là
input do fuzz tìm ra và được giữ nguyên. --check so tám tệp đã commit với bản sinh lại.

Chạy: python tools/gen_pak_seeds.py [--check]
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from collections.abc import Sequence
from pathlib import Path

OUTPUT = Path(__file__).resolve().parent.parent / "tests/fuzz/corpus/io_pak_index"

HEADER_SIZE = 64
ZSTD_MAGIC = 0xFD2FB528

# (đường dẫn, blob, original_size, compression, dictionary)
Entry = tuple[str, bytes, int, int, int]


def blake2b_256(data: bytes) -> bytes:
    return hashlib.blake2b(data, digest_size=32).digest()


def raw_frame(payload: bytes) -> bytes:
    """Frame Zstd gồm một khối raw: Frame_Header_Descriptor 0x20 bật Single_Segment_flag nên
    Frame_Content_Size chiếm 1 byte; Block_Header 3 byte = Last_Block | Raw_Block << 1 | cỡ << 3."""
    if len(payload) >= 256:
        raise ValueError("Frame_Content_Size 1 byte chỉ ghi được cỡ dưới 256")
    block_header = 1 | (len(payload) << 3)
    return (struct.pack("<IBB", ZSTD_MAGIC, 0x20, len(payload)) +
            block_header.to_bytes(3, "little") + payload)


def pak(entries: Sequence[Entry], dictionaries: Sequence[bytes] = (), gap: int = 0,
        version: int = 1) -> bytes:
    """Một pak đúng docs/formats/pak.md; `gap` byte 0 chèn trước mỗi blob."""
    ordered = sorted(entries, key=lambda entry: entry[0].encode())
    blobs = b""
    records = b""
    strings = b""
    for path, blob, original_size, compression, dictionary in ordered:
        offset = HEADER_SIZE + len(blobs) + gap
        blobs += b"\0" * gap + blob
        records += struct.pack("<QQQIHBB", offset, len(blob), original_size, len(strings),
                               len(path), compression, dictionary) + blake2b_256(blob)
        strings += path.encode()
    dictionary_records = b""
    dictionary_data = b""
    for content in dictionaries:
        dictionary_records += struct.pack("<II", len(dictionary_data), len(content))
        dictionary_data += content
    index = records + dictionary_records + strings + dictionary_data
    header = (b"ORIONPAK" +
              struct.pack("<HHIIIQQ", version, 0, len(ordered), len(dictionaries), len(strings),
                          HEADER_SIZE + len(blobs), len(index)) +
              b"\0" * 24)
    assert len(header) == HEADER_SIZE
    return header + blobs + index


def seeds() -> dict[str, bytes]:
    """Tên tệp → nội dung. pak_seed_test.cpp đọc đúng các tên này."""
    return {
        "empty": pak([]),
        "one_stored": pak([("a.bin", b"hello", 5, 0, 0)]),
        "nested_stored": pak([
            ("textures/stone_01.ktx2", b"\x01" * 40, 40, 0, 0),
            ("maps/zone-7/tile_3_4.nav", b"nav", 3, 0, 0),
            ("data/items.bin", b"", 0, 0, 0),
        ]),
        "zstd_frames": pak([
            ("a.txt", raw_frame(b"orion pak"), 9, 1, 0),
            ("b.txt", raw_frame(b"x" * 100), 100, 1, 0),
        ]),
        "dictionaries": pak([("a.txt", raw_frame(b"dict"), 4, 1, 1), ("b.bin", b"raw", 3, 0, 0)],
                            dictionaries=[b"d" * 300, b"e" * 64]),
        "gaps": pak([("a.bin", b"aaaa", 4, 0, 0), ("b.bin", b"bbbb", 4, 0, 0)], gap=16),
        "version_2": pak([("a.bin", b"a", 1, 0, 0)], version=2),
        "truncated_header": pak([("a.bin", b"a", 1, 0, 0)])[:40],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="không ghi; so các tệp đã commit với bản sinh lại")
    args = parser.parse_args(argv)
    generated = seeds()
    if args.check:
        stale = [name for name, content in generated.items()
                 if not (OUTPUT / name).is_file() or (OUTPUT / name).read_bytes() != content]
        if stale:
            print(f"gen_pak_seeds: {', '.join(stale)} khác bản sinh lại; chạy lại script",
                  file=sys.stderr)
            return 1
        print("gen_pak_seeds: khớp.")
        return 0
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, content in generated.items():
        (OUTPUT / name).write_bytes(content)
    print(f"gen_pak_seeds: đã ghi {len(generated)} tệp vào {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
