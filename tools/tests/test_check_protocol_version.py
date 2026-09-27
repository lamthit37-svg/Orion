"""Test cổng kiểm phiên bản protocol (tools/check_protocol_version.py; CLAUDE.md X.10): mỗi test
dựng lịch sử git trong một repo giả, rồi so cây làm việc với một commit gốc."""

from __future__ import annotations

import subprocess
import unittest

import check_protocol_version as gate
from tests import support

VERSION = "# Phiên bản.\nversion {n}\n"
CHAT = """message ChatSend = 20 {{
    channel reliable_ordered
    from client
    rate 2/s burst 5
    text: string[{size}]
}}
"""


class ProtocolVersionTest(support.FakeRepo):
    def setUp(self) -> None:
        super().setUp()
        self.git("config", "user.name", "test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "commit.gpgsign", "false")

    def protocol(self, version: int = 1, size: int = 255, **extra: str) -> None:
        self.write("game/shared/protocol/version.schema", VERSION.format(n=version))
        self.write("game/shared/protocol/chat.schema", CHAT.format(size=size))
        for name, text in extra.items():
            self.write(f"game/shared/protocol/{name}.schema", text)

    def commit(self) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", "commit")
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.root, check=True,
                              capture_output=True)
        return head.stdout.decode("utf-8").strip()

    def check(self, base: str, fallback: str | None = None) -> tuple[int, str]:
        args = ["--root", str(self.root), "--base", base]
        if fallback is not None:
            args += ["--fallback", fallback]
        return self.capture(lambda: gate.main(args))

    def expect_pass(self, base: str) -> None:
        code, out = self.check(base)
        self.assertEqual(code, 0, out)
        self.assertIn("check_protocol_version: xanh.", out)

    def expect_version_error(self, base: str, message: str) -> None:
        code, out = self.check(base)
        self.assertEqual(code, 1, out)
        self.assertEqual(self.rules(out),
                         [("game/shared/protocol/version.schema", "protocol/version")])
        # Lỗi chỉ đúng số version cần sửa: dòng 2, cột 9 của VERSION.
        self.assertIn("game/shared/protocol/version.schema:2:9: lỗi [protocol/version]: ", out)
        self.assertIn(message, out)

    def test_protocol_that_did_not_exist_at_the_base_passes(self) -> None:
        base = self.commit()
        self.protocol()
        self.expect_pass(base)

    def test_unchanged_protocol_passes(self) -> None:
        self.protocol()
        base = self.commit()
        self.expect_pass(base)

    def test_comments_and_file_layout_need_no_bump(self) -> None:
        self.protocol()
        base = self.commit()
        # Comment mới, và cùng protocol viết thành một tệp khác tên.
        (self.root / "game/shared/protocol/chat.schema").unlink()
        rewritten = CHAT.format(size=255).replace("    text", "    # nội dung\n    text")
        self.write("game/shared/protocol/talk.schema", "# Chat, viết lại.\n" + rewritten)
        self.expect_pass(base)

    def test_every_change_of_meaning_needs_a_bump(self) -> None:
        self.protocol()
        base = self.commit()
        chat = CHAT.format(size=255)
        changes = {
            "trường mới": chat.replace("    text:", "    loud: bool\n    text:"),
            "cỡ tối đa": chat.replace("string[255]", "string[200]"),
            "id": chat.replace("= 20", "= 21"),
            "kênh": chat.replace("reliable_ordered", "reliable_unordered"),
            "giới hạn tần suất": chat.replace("rate 2/s", "rate 3/s"),
            "tên": chat.replace("ChatSend", "ChatSay"),
        }
        for what, text in changes.items():
            with self.subTest(what):
                self.write("game/shared/protocol/chat.schema", text)
                self.expect_version_error(base, "mà version vẫn là 1")
                self.write("game/shared/protocol/version.schema", VERSION.format(n=2))
                self.expect_pass(base)
                self.write("game/shared/protocol/version.schema", VERSION.format(n=1))

    def test_version_never_decreases(self) -> None:
        self.protocol(version=5)
        base = self.commit()
        self.protocol(version=4)
        self.expect_version_error(base, "version giảm từ 5 xuống 4")
        self.protocol(version=4, size=100)
        self.expect_version_error(base, "version giảm từ 5 xuống 4")

    def test_a_bump_without_change_passes(self) -> None:
        self.protocol(version=1)
        base = self.commit()
        self.protocol(version=2)
        self.expect_pass(base)

    def test_a_base_the_current_codegen_rejects_counts_as_changed(self) -> None:
        # Struct không ai dùng: codegen hiện tại từ chối, nên không so được nghĩa.
        self.protocol(extra="struct Extra { a: bool }\n")
        base = self.commit()
        (self.root / "game/shared/protocol/extra.schema").unlink()
        self.expect_version_error(base, "mà version vẫn là 1")
        self.protocol(version=2)
        self.expect_pass(base)

    def test_a_broken_schema_in_the_tree_is_reported(self) -> None:
        self.protocol()
        base = self.commit()
        self.write("game/shared/protocol/chat.schema", "message {")
        code, out = self.check(base)
        self.assertEqual(code, 1)
        self.assertIn("game/shared/protocol/chat.schema:1:9: lỗi [schema]: ", out)

    def test_a_new_branch_falls_back_to_the_merge_base(self) -> None:
        self.protocol()
        self.commit()
        self.git("branch", "-M", "main")
        self.git("checkout", "-q", "-b", "feature")
        self.protocol(size=100)
        self.commit()
        zeros = "0" * 40
        code, out = self.check(zeros, fallback="main")
        self.assertEqual(code, 1, out)
        self.assertIn("mà version vẫn là 1", out)
        self.protocol(version=2, size=100)
        code, out = self.check("", fallback="main")
        self.assertEqual(code, 0, out)

    def test_a_missing_base_is_an_error(self) -> None:
        self.protocol()
        self.commit()
        with self.assertRaises(SystemExit) as caught:
            self.check("0" * 40)
        self.assertIn("không có commit gốc", str(caught.exception))
        with self.assertRaises(SystemExit):
            self.check("khong-co-nhanh-nay", fallback="cung-khong-co")


class DeclaredVersionTest(unittest.TestCase):
    def test_reads_a_single_declaration(self) -> None:
        self.assertEqual(gate.declared_version({"a": "# version 9\nversion 3\n"}), 3)
        self.assertIsNone(gate.declared_version({"a": "version 3\n", "b": "  version 4\n"}))
        self.assertIsNone(gate.declared_version({"a": "message A = 1 {}\n"}))


if __name__ == "__main__":
    unittest.main()
