#!/usr/bin/env python3
"""Cổng kiểm tầng và include của Orion (CLAUDE.md X.1, X.2, X.11; ARCH §3).

Bắt năm lỗi mà CLAUDE.md X.1 liệt kê, cộng hai lỗi giữ cho năm luật đó kiểm được:

- `layers/tier`, `layers/product`, `layers/cycle`: include đi ngược tầng, phụ thuộc vào `tools/`,
  `game/bot` hay một sản phẩm T5 khác, hoặc vòng phụ thuộc giữa các module;
- `layers/detail`: include vào `detail/` của module khác;
- `layers/platform-macro`, `layers/platform-define`, `layers/os-header`: macro hay header nền tảng
  dùng ngoài thư mục nền tảng (`win/`, `linux/`, `android/`, `apple/`) và ngoài
  `engine/core/platform.hpp`;
- `layers/external`, `layers/external-public`, `layers/external-unknown`: header thư viện ngoài
  dùng ngoài module bọc nó, lộ ra header public, hoặc chưa được gán module bọc;
- `layers/banned-shared`: header bị cấm trong `game/shared`;
- `layers/include-path`, `layers/unknown-module`: include dự án không đi từ gốc repo, hoặc nằm
  trong một module không có ở ARCH §3.

Gọi tay: `py tools/check_layers.py` trên Windows, `python3.13 tools/check_layers.py` trên Linux.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import sys
from collections.abc import Iterable

import gatelib

# ARCH §3. Đổi bảng này là đổi tầng: cần ADR (CLAUDE.md X.2).
ENGINE_TIERS = {
    "core": 0, "math": 0,
    "jobs": 1, "crypto": 1, "io": 1, "net": 1, "asset": 1, "physics": 1, "anim": 1, "nav": 1,
    "script": 1,
    "platform": 2, "rhi": 2, "render": 2, "audio": 2, "ui": 2,
}

PLATFORM_DIRS = frozenset({"win", "linux", "android", "apple"})
RHI_API_DIRS = frozenset({"d3d12", "vulkan", "metal"})
PLATFORM_HEADER = "engine/core/platform.hpp"

# Macro do compiler định nghĩa để nhận nền tảng, compiler, kiến trúc. Chỉ platform.hpp và thư mục
# nền tảng được đọc chúng; phần còn lại dùng ORION_COMPILER_* và ORION_ARCH_* do platform.hpp đặt.
RAW_PLATFORM_MACROS = frozenset(
    {
        "_WIN32", "_WIN64", "WINAPI_FAMILY", "__MINGW32__", "__MINGW64__", "__CYGWIN__",
        "__linux__", "__linux", "__gnu_linux__", "__ANDROID__", "__APPLE__", "__MACH__",
        "TARGET_OS_IPHONE", "TARGET_OS_IOS", "TARGET_OS_OSX", "TARGET_OS_MAC",
        "TARGET_OS_SIMULATOR",
        "__unix__", "__unix", "__FreeBSD__", "__EMSCRIPTEN__",
        "_MSC_VER", "_MSC_FULL_VER", "__clang__", "__GNUC__", "__clang_major__",
        "__x86_64__", "__amd64__", "_M_X64", "_M_AMD64", "__i386__", "_M_IX86",
        "__aarch64__", "_M_ARM64", "__arm__", "_M_ARM",
    }
)
_RAW_MACRO_PATTERN = re.compile(r"\b(" + "|".join(sorted(RAW_PLATFORM_MACROS)) + r")\b")
_ORION_PLATFORM_PATTERN = re.compile(r"\bORION_PLATFORM_[A-Z0-9_]+\b")
_ORION_DEFINE_PATTERN = re.compile(
    r"^[ \t]*#[ \t]*(?:define|undef)[ \t]+(ORION_(?:PLATFORM|COMPILER|ARCH)_[A-Z0-9_]+)",
    re.MULTILINE,
)
_INCLUDE_PATTERN = re.compile(
    r"^[ \t]*#[ \t]*(?:include|include_next|import)[ \t]*([<\"])([^>\"\n]+)[>\"]", re.MULTILINE
)

CPP_STD_HEADERS = frozenset(
    """
    algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono
    cinttypes climits clocale cmath codecvt compare complex concepts condition_variable coroutine
    csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype
    deque exception execution expected filesystem flat_map flat_set format forward_list fstream
    functional future generator initializer_list iomanip ios iosfwd iostream istream iterator latch
    limits list locale map mdspan memory memory_resource mutex new numbers numeric optional ostream
    print queue random ranges ratio regex scoped_allocator semaphore set shared_mutex
    source_location span spanstream sstream stack stacktrace stdexcept stdfloat stop_token
    streambuf string string_view syncstream system_error thread tuple type_traits typeindex
    typeinfo unordered_map unordered_set utility valarray variant vector version
    assert.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h
    signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h
    stdnoreturn.h string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h
    """.split()
)
# Intrinsic của compiler: phụ thuộc kiến trúc chứ không phụ thuộc nền tảng.
INTRINSIC_HEADERS = frozenset(
    """
    immintrin.h xmmintrin.h emmintrin.h pmmintrin.h tmmintrin.h smmintrin.h nmmintrin.h
    wmmintrin.h arm_neon.h arm_acle.h intrin.h
    """.split()
)
OS_HEADERS = frozenset(
    """
    windows.h winsock2.h ws2tcpip.h mswsock.h mstcpip.h mmsystem.h timeapi.h avrt.h bcrypt.h
    dbghelp.h
    psapi.h processthreadsapi.h synchapi.h winternl.h objbase.h combaseapi.h shellapi.h shlobj.h
    knownfolders.h wrl.h unistd.h fcntl.h pthread.h dlfcn.h poll.h sched.h netdb.h ifaddrs.h
    syslog.h termios.h spawn.h pwd.h grp.h execinfo.h malloc.h alloca.h jni.h process.h
    TargetConditionals.h
    """.split()
)
OS_HEADER_PREFIXES = (
    "sys/", "netinet/", "arpa/", "linux/", "android/", "mach/", "mach-o/", "libkern/", "os/",
    "dispatch/", "CoreFoundation/", "wrl/",
)

# Header thư viện ngoài -> nơi được include nó (CLAUDE.md X.2). Thêm thư viện mới là thêm một dòng ở
# đây trong cùng commit với ADR của dependency đó.
EXTERNAL_OWNERS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("sodium.h", ("engine/crypto",)),
    ("sodium/", ("engine/crypto",)),
    ("zstd.h", ("engine/io",)),
    ("zstd_errors.h", ("engine/io",)),
    ("zdict.h", ("engine/io",)),
    ("Jolt/", ("engine/physics",)),
    ("ozz/", ("engine/anim",)),
    ("Detour", ("engine/nav",)),
    ("Recast", ("engine/nav",)),
    ("lua.h", ("engine/script",)),
    ("lualib.h", ("engine/script",)),
    ("luacode.h", ("engine/script",)),
    ("Luau/", ("engine/script",)),
    ("SDL3/", ("engine/platform",)),
    ("d3d12", ("engine/rhi/d3d12",)),
    ("dxgi", ("engine/rhi/d3d12",)),
    ("D3D12MemAlloc.h", ("engine/rhi/d3d12",)),
    ("directx/", ("engine/rhi/d3d12",)),
    ("vulkan/", ("engine/rhi/vulkan",)),
    ("volk.h", ("engine/rhi/vulkan",)),
    ("vk_mem_alloc.h", ("engine/rhi/vulkan",)),
    ("Metal/", ("engine/rhi/metal",)),
    ("QuartzCore/", ("engine/rhi/metal",)),
    ("Foundation/", ("engine/rhi/metal",)),
    ("fmod", ("engine/audio",)),
    ("RmlUi/", ("engine/ui",)),
    ("ft2build.h", ("engine/ui",)),
    ("freetype/", ("engine/ui",)),
    ("hb.h", ("engine/ui",)),
    ("hb-", ("engine/ui",)),
    ("harfbuzz/", ("engine/ui",)),
    ("libpq-fe.h", ("game/server/lib/db",)),
    ("libpq/", ("game/server/lib/db",)),
    ("boost/", ("game/server/lib/http",)),
    ("simdjson.h", ("game/server/lib/http",)),
    ("entt/", ("game/server/world", "game/server/instance")),
)
# Thư viện ngoài được dùng thẳng trong header của module sở hữu (ARCH §4.5: EnTT).
EXTERNAL_PUBLIC_ALLOWED = ("entt/",)
TEST_FRAMEWORK_PREFIXES = ("gtest/", "gmock/", "benchmark/")

# CLAUDE.md X.11: game/shared không đọc đồng hồ, không tạo luồng, không dùng RNG toàn cục, không
# làm I/O.
SHARED_BANNED = frozenset(
    """
    chrono thread random fstream cmath math.h ctime time.h iostream istream ostream filesystem
    future cstdio stdio.h print syncstream mutex shared_mutex condition_variable stop_token
    semaphore latch barrier execution threads.h
    """.split()
)

PROJECT_ROOTS = ("engine/", "game/", "tools/", "tests/")
SCANNED_ROOTS = PROJECT_ROOTS


@dataclasses.dataclass(frozen=True)
class Module:
    """Một module theo ARCH §2 và §3: tên là thư mục, kind quyết định luật tầng."""

    name: str
    kind: str  # engine | shared | server_lib | server | client | bot | tool | tests
    tier: int


def module_of(path: str) -> Module | str | None:
    """Module chứa một đường dẫn; chuỗi là thông điệp lỗi; None là ngoài vùng kiểm."""
    parts = path.split("/")
    head = parts[0]
    if head == "engine" and len(parts) >= 3:
        tier = ENGINE_TIERS.get(parts[1])
        if tier is None:
            return f"engine/{parts[1]} không có trong ARCH §3"
        return Module(f"engine/{parts[1]}", "engine", tier)
    if head == "game" and len(parts) >= 3:
        if parts[1] == "shared":
            return Module("game/shared", "shared", 3)
        if parts[1] in ("client", "bot"):
            return Module(f"game/{parts[1]}", parts[1], 5)
        if parts[1] == "server" and len(parts) >= 4:
            if parts[2] == "lib":
                if len(parts) < 5:
                    return "tệp phải nằm trong một module của game/server/lib/"
                return Module(f"game/server/lib/{parts[3]}", "server_lib", 4)
            return Module(f"game/server/{parts[2]}", "server", 5)
        return f"game/{parts[1]} không có trong ARCH §2"
    if head == "tools" and len(parts) >= 3:
        return Module(f"tools/{parts[1]}", "tool", 5)
    if head == "tests" and len(parts) >= 3:
        return Module(f"tests/{parts[1]}", "tests", 6)
    return None


def tier_message(src: Module, dst: Module) -> str:
    return f"{src.name} (T{src.tier}) không được include {dst.name} (T{dst.tier}), xem ARCH §3"


def dependency_error(src: Module, dst: Module) -> tuple[str, str] | None:
    """Lý do src không được include dst theo ARCH §3, hoặc None nếu được."""
    if src.name == dst.name:
        return None
    if dst.kind == "tool":
        return "layers/product", f"không ai phụ thuộc tools/ ({src.name} -> {dst.name})"
    if src.kind == "tests":
        return None
    if dst.kind in ("server", "client", "bot", "tests"):
        return "layers/product", f"{src.name} không được phụ thuộc sản phẩm T5 {dst.name}"
    allowed_engine_tier = {
        "engine": src.tier, "shared": 1, "server_lib": 1, "server": 1, "client": 2, "bot": 1,
        "tool": 2,
    }[src.kind]
    if dst.kind == "engine":
        if dst.tier > allowed_engine_tier:
            return "layers/tier", tier_message(src, dst)
        return None
    allowed_kinds = {
        "engine": (),
        "shared": ("shared",),
        "server_lib": ("shared", "server_lib"),
        "server": ("shared", "server_lib"),
        "client": ("shared",),
        "bot": ("shared",),
        "tool": ("shared", "server_lib"),
    }[src.kind]
    if dst.kind not in allowed_kinds:
        return "layers/tier", tier_message(src, dst)
    return None


def in_platform_dir(path: str) -> bool:
    return any(part in PLATFORM_DIRS for part in path.split("/")[:-1])


def is_internal_location(path: str) -> bool:
    """detail/, thư mục nền tảng và thư mục API của RHI không phải header public."""
    dirs = path.split("/")[:-1]
    return "detail" in dirs or any(d in PLATFORM_DIRS or d in RHI_API_DIRS for d in dirs)


def in_tests_dir(path: str) -> bool:
    return path.startswith("tests/") or "tests" in path.split("/")[:-1]


def external_owners(header: str) -> tuple[str, ...] | None:
    for prefix, owners in EXTERNAL_OWNERS:
        if header.startswith(prefix):
            return owners
    return None


def is_os_header(header: str) -> bool:
    return header in OS_HEADERS or header.startswith(OS_HEADER_PREFIXES)


@dataclasses.dataclass
class Scan:
    findings: list[gatelib.Finding] = dataclasses.field(default_factory=list)
    # Cạnh module -> module, kèm một vị trí include làm ví dụ khi báo vòng.
    edges: dict[tuple[str, str], tuple[str, int]] = dataclasses.field(default_factory=dict)

    def add(self, path: str, line: int, rule: str, message: str) -> None:
        self.findings.append(gatelib.Finding(path, line, 1, rule, message))


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def check_angle_include(scan: Scan, path: str, line: int, header: str, src: Module) -> None:
    if header in CPP_STD_HEADERS or header in INTRINSIC_HEADERS:
        if src.kind == "shared" and header in SHARED_BANNED:
            scan.add(path, line, "layers/banned-shared",
                     f"<{header}> bị cấm trong game/shared (X.11)")
        return
    if header.startswith(PROJECT_ROOTS):
        scan.add(path, line, "layers/include-path", f"header dự án phải include bằng \"{header}\"")
        return
    if is_os_header(header):
        if not (in_platform_dir(path) or path == PLATFORM_HEADER):
            scan.add(path, line, "layers/os-header",
                     f"<{header}> là API hệ điều hành; chỉ dùng trong win/, linux/, android/, "
                     "apple/")
        return
    if in_tests_dir(path) and header.startswith(TEST_FRAMEWORK_PREFIXES):
        return
    if src.kind == "tool":
        return  # CLAUDE.md X.2: tools/ không bị ràng buộc bởi luật module bọc.
    owners = external_owners(header)
    if owners is None:
        scan.add(path, line, "layers/external-unknown",
                 f"<{header}> chưa được gán module bọc; thêm vào EXTERNAL_OWNERS kèm ADR "
                 "dependency")
        return
    if not any(path.startswith(owner + "/") for owner in owners):
        scan.add(path, line, "layers/external",
                 f"<{header}> chỉ được include trong {', '.join(owners)} (X.2)")
        return
    public_header = pathlib.PurePosixPath(path).suffix in gatelib.HEADER_SUFFIXES
    if public_header and not is_internal_location(path) and not header.startswith(
        EXTERNAL_PUBLIC_ALLOWED
    ):
        scan.add(path, line, "layers/external-public",
                 f"header public không được lộ <{header}>; include nó trong .cpp hoặc detail/")


def check_quote_include(scan: Scan, path: str, line: int, header: str, src: Module) -> None:
    if not header.startswith(PROJECT_ROOTS):
        scan.add(path, line, "layers/include-path",
                 f"\"{header}\": include dự án phải đi từ gốc repo, ví dụ "
                 "\"engine/core/types.hpp\"")
        return
    dst = module_of(header)
    if isinstance(dst, str):
        scan.add(path, line, "layers/unknown-module", dst)
        return
    if dst is None:
        return
    if "detail" in header.split("/")[:-1] and dst.name != src.name:
        scan.add(path, line, "layers/detail", f"không include detail/ của module khác ({dst.name})")
    error = dependency_error(src, dst)
    if error:
        scan.add(path, line, *error)
    if dst.name != src.name and src.kind != "tests":
        scan.edges.setdefault((src.name, dst.name), (path, line))


def check_macros(scan: Scan, path: str, code: str) -> None:
    for match in _ORION_DEFINE_PATTERN.finditer(code):
        if path != PLATFORM_HEADER:
            scan.add(path, line_of(code, match.start()), "layers/platform-define",
                     f"{match.group(1)} chỉ được định nghĩa trong {PLATFORM_HEADER}")
    if path == PLATFORM_HEADER or in_platform_dir(path):
        return
    for pattern, hint in (
        (_RAW_MACRO_PATTERN, "dùng ORION_COMPILER_* hoặc ORION_ARCH_* của platform.hpp"),
        (_ORION_PLATFORM_PATTERN, "chuyển code này vào thư mục win/, linux/, android/, apple/"),
    ):
        for match in pattern.finditer(code):
            scan.add(path, line_of(code, match.start()), "layers/platform-macro",
                     f"macro nền tảng {match.group(0)} ngoài thư mục nền tảng; {hint}")


def scan_file(scan: Scan, root: pathlib.Path, path: str) -> None:
    src = module_of(path)
    if isinstance(src, str):
        scan.add(path, 1, "layers/unknown-module", src)
        return
    if src is None:
        return
    text = gatelib.read_text(root / path)
    with_strings = gatelib.strip_cpp(text, keep_strings=True)
    for match in _INCLUDE_PATTERN.finditer(with_strings):
        line = line_of(with_strings, match.start())
        delim, header = match.group(1), match.group(2).strip()
        if delim == "<":
            check_angle_include(scan, path, line, header, src)
        else:
            check_quote_include(scan, path, line, header, src)
    check_macros(scan, path, gatelib.strip_cpp(text, keep_strings=False))


def find_cycles(edges: Iterable[tuple[str, str]]) -> list[list[str]]:
    """Thành phần liên thông mạnh có hơn một module (Tarjan, viết lặp để không đệ quy)."""
    graph: dict[str, list[str]] = {}
    for a, b in edges:
        graph.setdefault(a, []).append(b)
        graph.setdefault(b, [])
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    on_stack: set[str] = set()
    stack: list[str] = []
    cycles: list[list[str]] = []
    counter = 0
    for start in sorted(graph):
        if start in index:
            continue
        work: list[tuple[str, int]] = [(start, 0)]
        while work:
            node, child = work.pop()
            if child == 0:
                index[node] = low[node] = counter
                counter += 1
                stack.append(node)
                on_stack.add(node)
            neighbours = sorted(graph[node])
            if child < len(neighbours):
                work.append((node, child + 1))
                nxt = neighbours[child]
                if nxt not in index:
                    work.append((nxt, 0))
                elif nxt in on_stack:
                    low[node] = min(low[node], index[nxt])
                continue
            if low[node] == index[node]:
                component: list[str] = []
                while True:
                    top = stack.pop()
                    on_stack.discard(top)
                    component.append(top)
                    if top == node:
                        break
                if len(component) > 1:
                    cycles.append(sorted(component))
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
    return cycles


def run(root: pathlib.Path, explicit: list[str] | None) -> int:
    scan = Scan()
    # Vòng phụ thuộc chỉ thấy được khi nhìn cả repo, nên luôn quét hết rồi mới lọc kết quả.
    all_files = [
        p
        for p in gatelib.list_source_files(root)
        if p.startswith(SCANNED_ROOTS)
        and pathlib.PurePosixPath(p).suffix in gatelib.CPP_SUFFIXES
    ]
    for path in all_files:
        scan_file(scan, root, path)
    for component in find_cycles(scan.edges):
        members = set(component)
        where = sorted(v for (a, b), v in scan.edges.items() if a in members and b in members)
        path, line = where[0]
        scan.add(path, line, "layers/cycle", "vòng phụ thuộc giữa " + ", ".join(component))
    wanted = set(gatelib.select(all_files, explicit)) if explicit else None
    findings = [f for f in scan.findings if wanted is None or f.path in wanted]
    return gatelib.report(findings, "check_layers")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("paths", nargs="*", help="chỉ báo lỗi của các tệp hoặc thư mục này")
    parser.add_argument("--root", type=pathlib.Path, help="gốc repo; mặc định tự dò")
    args = parser.parse_args(argv)
    root = args.root.resolve() if args.root else gatelib.repo_root()
    return run(root, args.paths)


if __name__ == "__main__":
    sys.exit(main())
