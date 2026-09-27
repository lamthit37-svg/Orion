"""Test cổng tầng: mỗi luật có ca đỏ và ca xanh, trên một repo giả."""

from __future__ import annotations

import unittest

import check_layers

from tests.support import FakeRepo


class DependencyRulesTest(unittest.TestCase):
    """Bảng tầng của ARCH §3, kiểm trực tiếp không cần tệp."""

    def module(self, path: str) -> check_layers.Module:
        found = check_layers.module_of(path)
        assert isinstance(found, check_layers.Module), found
        return found

    def allowed(self, src: str, dst: str) -> bool:
        return check_layers.dependency_error(self.module(src), self.module(dst)) is None

    def test_engine_tiers(self) -> None:
        self.assertTrue(self.allowed("engine/net/a.cpp", "engine/core/b.hpp"))
        self.assertTrue(self.allowed("engine/net/a.cpp", "engine/crypto/b.hpp"))
        self.assertFalse(self.allowed("engine/core/a.cpp", "engine/net/b.hpp"))
        self.assertFalse(self.allowed("engine/math/a.cpp", "engine/jobs/b.hpp"))
        self.assertTrue(self.allowed("engine/render/a.cpp", "engine/rhi/b.hpp"))
        self.assertFalse(self.allowed("engine/core/a.cpp", "game/shared/b.hpp"))

    def test_shared_never_sees_t2(self) -> None:
        self.assertTrue(self.allowed("game/shared/a.cpp", "engine/net/b.hpp"))
        self.assertFalse(self.allowed("game/shared/a.cpp", "engine/render/b.hpp"))
        self.assertFalse(self.allowed("game/shared/a.cpp", "game/server/lib/db/b.hpp"))

    def test_server_library_and_products(self) -> None:
        self.assertTrue(self.allowed("game/server/lib/ledger/a.cpp", "game/server/lib/db/b.hpp"))
        self.assertFalse(self.allowed("game/server/lib/db/a.cpp", "engine/rhi/b.hpp"))
        self.assertTrue(self.allowed("game/server/auth/a.cpp", "game/server/lib/http/b.hpp"))
        self.assertFalse(self.allowed("game/server/auth/a.cpp", "game/server/gateway/b.hpp"))
        self.assertFalse(self.allowed("game/server/auth/a.cpp", "engine/platform/b.hpp"))
        self.assertFalse(self.allowed("game/client/a.cpp", "game/server/lib/db/b.hpp"))
        self.assertTrue(self.allowed("game/client/a.cpp", "engine/render/b.hpp"))
        self.assertFalse(self.allowed("game/bot/a.cpp", "engine/render/b.hpp"))
        self.assertFalse(self.allowed("game/bot/a.cpp", "game/client/b.hpp"))

    def test_tools_and_tests(self) -> None:
        self.assertTrue(self.allowed("tools/cooker/a.cpp", "engine/render/b.hpp"))
        self.assertTrue(self.allowed("tools/migrate/a.cpp", "game/server/lib/db/b.hpp"))
        self.assertFalse(self.allowed("tools/editor/a.cpp", "tools/cooker/b.hpp"))
        self.assertFalse(self.allowed("tools/editor/a.cpp", "game/client/b.hpp"))
        self.assertFalse(self.allowed("game/server/world/a.cpp", "tools/cooker/b.hpp"))
        self.assertTrue(self.allowed("tests/fuzz/a.cpp", "game/server/auth/b.hpp"))
        self.assertFalse(self.allowed("tests/fuzz/a.cpp", "tools/cooker/b.hpp"))

    def test_unknown_modules(self) -> None:
        self.assertIsInstance(check_layers.module_of("engine/magic/a.cpp"), str)
        self.assertIsInstance(check_layers.module_of("game/other/a.cpp"), str)
        self.assertIsNone(check_layers.module_of("docs/a.cpp"))


class ScanTest(FakeRepo):
    def run_gate(self) -> tuple[int, list[tuple[str, str]]]:
        code, output = self.capture(lambda: check_layers.main(["--root", str(self.root)]))
        return code, self.rules(output)

    def assert_findings(self, expected: list[tuple[str, str]]) -> None:
        code, found = self.run_gate()
        self.assertEqual(sorted(found), sorted(expected))
        self.assertEqual(code, 1 if expected else 0)

    def test_clean_layering_passes(self) -> None:
        self.write("engine/core/types.hpp", "#pragma once\n#include <cstdint>\n")
        self.write("engine/net/net.cpp", '#include "engine/core/types.hpp"\n#include <vector>\n')
        self.write("game/shared/move.cpp", '#include "engine/net/net.hpp"\n#include <array>\n')
        self.assert_findings([])

    def test_upward_include_and_product_dependency(self) -> None:
        self.write("engine/core/a.cpp", '#include "engine/net/b.hpp"\n')
        self.write("game/server/auth/a.cpp", '#include "game/server/gateway/b.hpp"\n')
        self.assert_findings(
            [("engine/core/a.cpp", "layers/tier"), ("game/server/auth/a.cpp", "layers/product")]
        )

    def test_detail_of_other_module(self) -> None:
        self.write("engine/net/a.cpp", '#include "engine/crypto/detail/impl.hpp"\n')
        self.write("engine/crypto/tests/t.cpp", '#include "engine/crypto/detail/impl.hpp"\n')
        self.assert_findings([("engine/net/a.cpp", "layers/detail")])

    def test_cycle_between_modules(self) -> None:
        self.write("engine/io/a.cpp", '#include "engine/net/b.hpp"\n')
        self.write("engine/net/b.cpp", '#include "engine/io/a.hpp"\n')
        code, found = self.run_gate()
        self.assertEqual(code, 1)
        self.assertEqual([rule for _, rule in found], ["layers/cycle"])

    def test_include_path_shape(self) -> None:
        self.write("engine/core/a.cpp", '#include "b.hpp"\n#include <engine/core/c.hpp>\n')
        self.assert_findings([("engine/core/a.cpp", "layers/include-path")] * 2)

    def test_external_headers_stay_in_wrapper(self) -> None:
        self.write("engine/crypto/crypto.cpp", "#include <sodium.h>\n")
        self.write("engine/crypto/detail/sodium_types.hpp", "#pragma once\n#include <sodium.h>\n")
        self.write("engine/crypto/crypto.hpp", "#pragma once\n#include <sodium.h>\n")
        self.write("engine/io/io.cpp", "#include <sodium.h>\n")
        self.write("engine/io/pak.cpp", "#include <mystery/lib.h>\n")
        self.assert_findings(
            [
                ("engine/crypto/crypto.hpp", "layers/external-public"),
                ("engine/io/io.cpp", "layers/external"),
                ("engine/io/pak.cpp", "layers/external-unknown"),
            ]
        )

    def test_external_exceptions(self) -> None:
        self.write("tools/cooker/a.cpp", "#include <mystery/lib.h>\n#include <sodium.h>\n")
        self.write("engine/core/tests/a_test.cpp", "#include <gtest/gtest.h>\n")
        self.write("game/server/world/zone.hpp", "#pragma once\n#include <entt/entt.hpp>\n")
        self.write("engine/rhi/d3d12/device.hpp", "#pragma once\n#include <d3d12.h>\n")
        self.write("engine/core/a.cpp", "#include <gtest/gtest.h>\n")
        self.assert_findings([("engine/core/a.cpp", "layers/external-unknown")])

    def test_os_headers_only_in_platform_dirs(self) -> None:
        self.write("engine/core/clock.cpp", "#include <windows.h>\n#include <sys/time.h>\n")
        self.write("engine/core/win/clock.cpp", "#include <windows.h>\n")
        self.write("engine/core/linux/clock.cpp", "#include <sys/time.h>\n")
        # <process.h> là API luồng của CRT Windows (_beginthreadex), không phải thư viện ngoài.
        self.write("engine/jobs/thread.cpp", "#include <process.h>\n")
        self.write("engine/jobs/win/thread.cpp", "#include <process.h>\n")
        # <mstcpip.h> (SIO_UDP_CONNRESET) là phần mở rộng của Winsock, không phải thư viện ngoài.
        self.write("engine/net/socket.cpp", "#include <mstcpip.h>\n")
        self.write("engine/net/win/socket.cpp", "#include <winsock2.h>\n#include <mstcpip.h>\n")
        self.assert_findings([("engine/core/clock.cpp", "layers/os-header")] * 2
                             + [("engine/jobs/thread.cpp", "layers/os-header"),
                                ("engine/net/socket.cpp", "layers/os-header")])

    def test_platform_macros(self) -> None:
        self.write("engine/core/a.cpp", "#ifdef _WIN32\n#endif\n#if ORION_PLATFORM_LINUX\n#endif\n")
        self.write("engine/core/win/a.cpp", "#ifdef _WIN32\n#endif\n")
        self.write("engine/math/simd.cpp", "#if ORION_ARCH_X64\n#endif\n")
        self.write("engine/math/b.cpp", '// _WIN32 trong comment\nconst char* s = "__linux__";\n')
        self.write("engine/core/platform.hpp", "#pragma once\n#define ORION_ARCH_X64 1\n")
        self.write("engine/core/c.cpp", "#define ORION_ARCH_ARM64 1\n")
        self.assert_findings(
            [
                ("engine/core/a.cpp", "layers/platform-macro"),
                ("engine/core/a.cpp", "layers/platform-macro"),
                ("engine/core/c.cpp", "layers/platform-define"),
            ]
        )

    def test_banned_headers_in_shared(self) -> None:
        self.write("game/shared/a.cpp", "#include <chrono>\n#include <cmath>\n#include <array>\n")
        self.write("engine/math/a.cpp", "#include <cmath>\n")
        self.assert_findings([("game/shared/a.cpp", "layers/banned-shared")] * 2)

    def test_unknown_module_is_reported(self) -> None:
        self.write("engine/magic/a.cpp", "int a;\n")
        self.assert_findings([("engine/magic/a.cpp", "layers/unknown-module")])


if __name__ == "__main__":
    unittest.main()
