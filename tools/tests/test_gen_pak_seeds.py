"""Test bộ ghi pak độc lập: bố cục theo docs/formats/pak.md, frame raw theo định dạng Zstd, và tệp
hạt giống đã commit còn khớp."""

from __future__ import annotations

import contextlib
import hashlib
import io
import struct
import unittest

import gen_pak_seeds as gen


class LayoutTest(unittest.TestCase):
    def test_header_points_at_an_index_that_ends_the_file(self) -> None:
        data = gen.pak([("a.bin", b"hello", 5, 0, 0)])
        magic, version, flags, entries, dictionaries, strings, index_offset, index_size = (
            struct.unpack_from("<8sHHIIIQQ", data))
        self.assertEqual((magic, version, flags), (b"ORIONPAK", 1, 0))
        self.assertEqual((entries, dictionaries, strings), (1, 0, 5))
        self.assertEqual(index_offset, 64 + 5)
        self.assertEqual(index_offset + index_size, len(data))
        self.assertEqual(data[40:64], b"\0" * 24)

    def test_entry_record_carries_offset_sizes_path_and_hash(self) -> None:
        data = gen.pak([("a.bin", b"hello", 5, 0, 0)])
        record = data[69:69 + 64]
        offset, stored, original, path_offset, path_length, compression, dictionary = (
            struct.unpack_from("<QQQIHBB", record))
        self.assertEqual((offset, stored, original), (64, 5, 5))
        self.assertEqual((path_offset, path_length, compression, dictionary), (0, 5, 0, 0))
        self.assertEqual(record[32:], hashlib.blake2b(b"hello", digest_size=32).digest())
        self.assertEqual(data[69 + 64:], b"a.bin")

    def test_entries_are_sorted_by_path_bytes(self) -> None:
        data = gen.pak([("b.bin", b"", 0, 0, 0), ("a.bin", b"", 0, 0, 0)])
        self.assertTrue(data.endswith(b"a.binb.bin"))

    def test_raw_frame_has_one_raw_block(self) -> None:
        # Magic, Frame_Header_Descriptor 0x20, Frame_Content_Size 3, Block_Header 1 | 3 << 3.
        self.assertEqual(gen.raw_frame(b"abc").hex(), "28b52ffd2003190000616263")
        with self.assertRaises(ValueError):
            gen.raw_frame(b"x" * 256)


class CommittedFileTest(unittest.TestCase):
    def test_committed_seeds_match_generator(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gen.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main()
