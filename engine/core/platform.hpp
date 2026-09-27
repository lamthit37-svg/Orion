#pragma once

// Nơi duy nhất định nghĩa macro nền tảng, compiler và kiến trúc (CLAUDE.md X.2).
//
// Mỗi macro luôn được định nghĩa với giá trị 0 hoặc 1, nên dùng bằng `#if`, không bằng `#ifdef`;
// -Wundef bắt chỗ gõ sai tên. Macro ORION_PLATFORM_* chỉ được dùng trong thư mục nền tảng (win/,
// linux/, android/, apple/); ORION_COMPILER_* và ORION_ARCH_* dùng được ở mọi nơi.
// tools/check_layers.py thi hành cả hai luật.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// ---------------------------------------------------------------------------------------------
// Hệ điều hành.
// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)
#define ORION_PLATFORM_WINDOWS 1
#else
#define ORION_PLATFORM_WINDOWS 0
#endif

#if defined(__ANDROID__)
#define ORION_PLATFORM_ANDROID 1
#else
#define ORION_PLATFORM_ANDROID 0
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#define ORION_PLATFORM_LINUX 1
#else
#define ORION_PLATFORM_LINUX 0
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#define ORION_PLATFORM_IOS 1
#else
#define ORION_PLATFORM_IOS 0
#endif

#if defined(__APPLE__) && !TARGET_OS_IPHONE
#define ORION_PLATFORM_MACOS 1
#else
#define ORION_PLATFORM_MACOS 0
#endif

static_assert(ORION_PLATFORM_WINDOWS + ORION_PLATFORM_ANDROID + ORION_PLATFORM_LINUX +
                      ORION_PLATFORM_IOS + ORION_PLATFORM_MACOS ==
                  1,
              "Orion chỉ build cho Windows, Linux, Android, iOS và macOS (công cụ)");

// ---------------------------------------------------------------------------------------------
// Compiler. clang-cl được tính là clang (ORION_COMPILER_CLANG) và có thêm ORION_COMPILER_CLANG_CL.
// ---------------------------------------------------------------------------------------------
#if defined(__clang__)
#define ORION_COMPILER_CLANG 1
#else
#define ORION_COMPILER_CLANG 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define ORION_COMPILER_MSVC 1
#else
#define ORION_COMPILER_MSVC 0
#endif

#if defined(_MSC_VER) && defined(__clang__)
#define ORION_COMPILER_CLANG_CL 1
#else
#define ORION_COMPILER_CLANG_CL 0
#endif

static_assert(ORION_COMPILER_CLANG + ORION_COMPILER_MSVC == 1,
              "Orion chỉ build bằng MSVC hoặc clang (CLAUDE.md X.12)");

// ---------------------------------------------------------------------------------------------
// Kiến trúc.
// ---------------------------------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#define ORION_ARCH_X64 1
#else
#define ORION_ARCH_X64 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define ORION_ARCH_ARM64 1
#else
#define ORION_ARCH_ARM64 0
#endif

static_assert(ORION_ARCH_X64 + ORION_ARCH_ARM64 == 1, "Orion chỉ build cho x64 và arm64");

// ---------------------------------------------------------------------------------------------
// Tiện ích phụ thuộc compiler.
// ---------------------------------------------------------------------------------------------

// Dừng tại chỗ khi có debugger; không có debugger thì tiến trình nhận tín hiệu bẫy.
#if ORION_COMPILER_MSVC
#define ORION_DEBUG_BREAK() __debugbreak()
#else
#define ORION_DEBUG_BREAK() __builtin_debugtrap()
#endif
