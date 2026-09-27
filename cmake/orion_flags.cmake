# orion_apply_flags(): cờ nền cho mọi target của dự án, trên cả năm toolchain (CLAUDE.md X.1, X.3,
# X.11, X.12; ARCH §6). Mọi target của engine/, game/, tools/ và tests/ đều phải đi qua hàm này;
# orion_add_module() và các hàm tạo target khác gọi nó thay người viết.
include_guard(GLOBAL)

# ---------------------------------------------------------------------------------------------
# Tuỳ chọn build. Preset đặt các giá trị này (ARCH §6); không đặt tay khi đã có preset phù hợp.
# ---------------------------------------------------------------------------------------------
set(ORION_SANITIZE "" CACHE STRING "Sanitizer cho mọi target: rỗng, address, undefined, thread")
set_property(CACHE ORION_SANITIZE PROPERTY STRINGS "" address undefined thread)
option(ORION_SHIP "Bản CHƠI: tắt ORION_ASSERT, công cụ dev và Tracy (CLAUDE.md X.9)" OFF)
option(ORION_PROFILE "Bật Tracy (preset profile)" OFF)
option(ORION_COVERAGE "Đo coverage bằng llvm-cov (preset linux-coverage)" OFF)
option(ORION_FUZZ "Dựng fuzz target với libFuzzer, ASan và UBSan (preset linux-fuzz)" OFF)

if(ORION_SHIP AND (ORION_PROFILE OR ORION_SANITIZE OR ORION_COVERAGE OR ORION_FUZZ))
    message(FATAL_ERROR "ORION_SHIP không đi cùng Tracy, sanitizer, coverage hay fuzz (X.9)")
endif()
if(ORION_FUZZ AND ORION_SANITIZE)
    message(FATAL_ERROR "ORION_FUZZ đã tự bật ASan và UBSan; bỏ ORION_SANITIZE")
endif()

# ---------------------------------------------------------------------------------------------
# Nhận toolchain. Chỉ năm toolchain ở CLAUDE.md X.12 được hỗ trợ; toolchain khác bị từ chối để
# không ai vô tình build bằng một compiler chưa đo.
# ---------------------------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(ORION_TOOLCHAIN "msvc")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(ORION_TOOLCHAIN "clang-cl")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    set(ORION_TOOLCHAIN "clang")
else()
    message(FATAL_ERROR
        "Compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} không thuộc năm toolchain "
        "của CLAUDE.md X.12 (MSVC, clang-cl, clang Linux, clang NDK, Apple clang).")
endif()

# NGHI-NGO-015: clang 18 với libstdc++ 13 không có std::expected; clang 20 thì có.
set(ORION_LINUX_CLANG_MIN_VERSION 20)
if(ORION_TOOLCHAIN STREQUAL "clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS ORION_LINUX_CLANG_MIN_VERSION)
    message(FATAL_ERROR
        "Toolchain 3 cần clang ${ORION_LINUX_CLANG_MIN_VERSION} trở lên; đang có "
        "${CMAKE_CXX_COMPILER_VERSION} (NGHI-NGO-015). Xem cmake/toolchains/linux-clang.cmake.")
endif()

if(ORION_SANITIZE STREQUAL "thread" AND NOT (ORION_TOOLCHAIN STREQUAL "clang"
                                            AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "TSan chỉ chạy với clang trên Linux (hiến pháp II.2, NGHI-NGO-004)")
endif()
if((ORION_FUZZ OR ORION_COVERAGE) AND NOT ORION_TOOLCHAIN STREQUAL "clang")
    message(FATAL_ERROR "Fuzz và coverage chỉ dựng bằng clang (preset linux-fuzz, linux-coverage)")
endif()

# ---------------------------------------------------------------------------------------------
# Python cho script của repo: cổng kiểm, codegen. CMake gọi script qua Python3_EXECUTABLE (ARCH).
# ---------------------------------------------------------------------------------------------
find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------------------------------------
# Thiết lập chung cho mọi target.
# ---------------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# C++20 modules bị cấm cho tới khi có ADR (X.12). Tắt quét module để Ninja không cần
# clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(ORION_SHIP)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()
if(ORION_TOOLCHAIN MATCHES "^(msvc|clang-cl)$")
    # CMake mặc định thêm /EHsc cho mọi target; exception do orion_apply_flags() quyết theo target.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
if(ORION_TOOLCHAIN STREQUAL "msvc" AND ORION_SANITIZE STREQUAL "address")
    # /RTC1 mặc định của Debug không chạy chung với ASan của MSVC.
    foreach(lang C CXX)
        string(REPLACE "/RTC1" "" CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
    endforeach()
endif()

# Cờ sàn của CLAUDE.md X.1, cộng cờ tất định (X.11) và cờ tắt exception, RTTI (X.3).
set(ORION_MSVC_WARNINGS
    /W4 /WX /utf-8 /Zc:preprocessor /external:W0
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
set(ORION_CLANG_WARNINGS
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion -Wshadow -Wundef
    -Wimplicit-fallthrough
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>)
set(ORION_CLANG_CL_WARNINGS
    /W4 /WX /utf-8 /external:W0
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    -Wconversion -Wsign-conversion -Wshadow -Wundef -Wimplicit-fallthrough
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>)

function(_orion_sanitizer_flags out_var)
    set(flags "")
    if(ORION_FUZZ)
        set(flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
                  -fno-sanitize=vptr -fno-omit-frame-pointer)
    elseif(ORION_SANITIZE STREQUAL "address")
        if(ORION_TOOLCHAIN STREQUAL "msvc")
            set(flags /fsanitize=address)
        else()
            set(flags -fsanitize=address -fno-omit-frame-pointer)
        endif()
    elseif(ORION_SANITIZE STREQUAL "undefined")
        # vptr cần RTTI, mà RTTI bị tắt (X.3). no-recover để UB làm test đỏ thay vì chỉ in ra.
        set(flags -fsanitize=undefined -fno-sanitize=vptr -fno-sanitize-recover=all)
        if(ORION_TOOLCHAIN STREQUAL "clang-cl")
            # NGHI-NGO-023: runtime UBSan của clang-cl chưa đo; bẫy thẳng không cần runtime.
            list(APPEND flags -fsanitize-trap=undefined)
        endif()
    elseif(ORION_SANITIZE STREQUAL "thread")
        set(flags -fsanitize=thread -fno-omit-frame-pointer)
    elseif(ORION_SANITIZE)
        message(FATAL_ERROR "ORION_SANITIZE không hợp lệ: ${ORION_SANITIZE}")
    endif()
    set(${out_var} "${flags}" PARENT_SCOPE)
endfunction()

# orion_apply_flags(<target> [EXCEPTIONS])
#
# EXCEPTIONS chỉ dành cho tools/ và module bọc thư viện bắt buộc exception, và exception phải được
# bắt hết ở ranh giới của module đó (CLAUDE.md X.3).
function(orion_apply_flags target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "EXCEPTIONS" "" "")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "orion_apply_flags: tham số lạ ${arg_UNPARSED_ARGUMENTS}")
    endif()

    if(ORION_TOOLCHAIN STREQUAL "msvc")
        target_compile_options(${target} PRIVATE ${ORION_MSVC_WARNINGS} /fp:precise)
        if(arg_EXCEPTIONS)
            target_compile_options(${target} PRIVATE /EHsc)
        else()
            target_compile_options(${target} PRIVATE /EHs-c- $<$<COMPILE_LANGUAGE:CXX>:/GR->)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        endif()
    elseif(ORION_TOOLCHAIN STREQUAL "clang-cl")
        # Không thêm /fp:precise: clang-cl dịch nó thành -ffp-model=precise, kéo theo
        # -ffp-contract=on, rồi báo -Woverriding-option khi gặp -ffp-contract=off (đo trên CI).
        # Mô hình mặc định của clang đã là precise.
        target_compile_options(${target} PRIVATE ${ORION_CLANG_CL_WARNINGS}
                                                 /clang:-ffp-contract=off)
        if(arg_EXCEPTIONS)
            target_compile_options(${target} PRIVATE /EHsc)
        else()
            target_compile_options(${target} PRIVATE /EHs-c- $<$<COMPILE_LANGUAGE:CXX>:/GR->)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        endif()
    else()
        target_compile_options(${target} PRIVATE ${ORION_CLANG_WARNINGS} -ffp-contract=off)
        if(NOT arg_EXCEPTIONS)
            target_compile_options(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
        endif()
        # ARCH §4.1: SIMD tới SSE4.2 trên x64. x86-64-v2 không có FMA nên không đổi kết quả số thực.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            target_compile_options(${target} PRIVATE -march=x86-64-v2)
        endif()
    endif()

    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    _orion_sanitizer_flags(sanitizer_flags)
    if(sanitizer_flags)
        target_compile_options(${target} PRIVATE ${sanitizer_flags})
        # MSVC tự suy ra thư viện ASan lúc link; clang-cl ở chế độ bẫy không cần runtime. Chỉ clang
        # với driver GNU cần cờ sanitizer ở bước link.
        if(ORION_TOOLCHAIN STREQUAL "clang")
            target_link_options(${target} PRIVATE ${sanitizer_flags})
        endif()
    endif()
    if(ORION_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    endif()

    # Macro cấu hình mang giá trị 0 hoặc 1 để dùng với #if; -Wundef bắt chỗ quên định nghĩa.
    target_compile_definitions(${target} PRIVATE
        ORION_SHIP=$<BOOL:${ORION_SHIP}>
        ORION_DEV_TOOLS=$<NOT:$<BOOL:${ORION_SHIP}>>
        ORION_PROFILE=$<BOOL:${ORION_PROFILE}>)
endfunction()
