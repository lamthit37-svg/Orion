# orion_add_module(): một module là một thư mục và một target (CLAUDE.md X.2, ARCH §2, §3).
#
# Tên target, loại module và tầng được suy ra từ đường dẫn thư mục, nên không thể đặt sai:
#   engine/<m>          -> engine_<m>   (tầng theo ORION_ENGINE_TIERS)
#   game/shared         -> game_shared  (T3)
#   game/server/lib/<m> -> server_<m>   (T4)
#   game/server/<tên>   -> server_<tên> (T5)
#   game/client         -> game_client  (T5)
#   game/bot            -> game_bot     (T5)
#   tools/<tên>         -> tool_<tên>   (T5)
#
# Quy ước tệp trong thư mục module:
#   *.hpp, *.cpp ở gốc       header public và phần cài đặt
#   <thư mục con>/*.hpp, *.cpp  chỉ game/shared: protocol/, movement/, combat/, defs/ (ARCH §2)
#   detail/                  thứ nội bộ; module khác không include
#   win/ linux/ android/ apple/  code riêng nền tảng, chỉ build trên nền tảng đó
#   tests/*_test.cpp         unit test (GoogleTest), chạy bằng ctest
#   tests/*_bench.cpp        benchmark (Google Benchmark); ctest chạy một lượt ngắn để chắc nó chạy
#   tests/support/           tiện ích test dùng chung, link vào test của module này và module sau
#   main.cpp                 không thuộc thư viện; orion_add_program() dùng nó
include_guard(GLOBAL)

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
include(GoogleTest)

# ARCH §3. Phải khớp ENGINE_TIERS trong tools/check_layers.py; tools/tests/test_tiers.py so hai
# bảng. Đổi bảng này là đổi tầng: cần ADR (CLAUDE.md X.2).
set(ORION_ENGINE_TIERS
    core=0 math=0
    jobs=1 crypto=1 io=1 net=1 asset=1 physics=1 anim=1 nav=1 script=1
    platform=2 rhi=2 render=2 audio=2 ui=2)

# Thư mục nền tảng được build cho hệ đích. Android dùng lại linux/ vì cùng kernel Linux và bionic có
# đủ API POSIX cần thiết; phần chỉ Android mới có nằm ở android/.
if(WIN32)
    set(ORION_PLATFORM_DIRS win)
elseif(ANDROID)
    set(ORION_PLATFORM_DIRS linux android)
elseif(APPLE)
    set(ORION_PLATFORM_DIRS apple)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ORION_PLATFORM_DIRS linux)
else()
    message(FATAL_ERROR "Hệ đích ${CMAKE_SYSTEM_NAME} không có thư mục nền tảng (ARCH §2)")
endif()

# Suy ra tên target, loại và tầng từ thư mục module.
function(_orion_module_identity dir out_target out_kind out_tier out_rel)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${dir}")
    string(REPLACE "/" ";" parts "${rel}")
    list(LENGTH parts depth)
    list(GET parts 0 head)
    set(target "")
    if(head STREQUAL "engine" AND depth EQUAL 2)
        list(GET parts 1 name)
        foreach(entry IN LISTS ORION_ENGINE_TIERS)
            string(REPLACE "=" ";" pair "${entry}")
            list(GET pair 0 module)
            list(GET pair 1 tier)
            if(module STREQUAL name)
                set(target "engine_${name}")
                set(kind "engine")
                break()
            endif()
        endforeach()
    elseif(rel STREQUAL "game/shared")
        set(target "game_shared")
        set(kind "shared")
        set(tier 3)
    elseif(rel STREQUAL "game/client" OR rel STREQUAL "game/bot")
        list(GET parts 1 name)
        set(target "game_${name}")
        set(kind "${name}")
        set(tier 5)
    elseif(head STREQUAL "game" AND depth EQUAL 4 AND rel MATCHES "^game/server/lib/")
        list(GET parts 3 name)
        set(target "server_${name}")
        set(kind "server_lib")
        set(tier 4)
    elseif(head STREQUAL "game" AND depth EQUAL 3 AND rel MATCHES "^game/server/")
        list(GET parts 2 name)
        set(target "server_${name}")
        set(kind "server")
        set(tier 5)
    elseif(head STREQUAL "tools" AND depth EQUAL 2)
        list(GET parts 1 name)
        set(target "tool_${name}")
        set(kind "tool")
        set(tier 5)
    endif()
    if(target STREQUAL "")
        message(FATAL_ERROR "${rel} không phải thư mục module của ARCH §2 và §3")
    endif()
    set(${out_target} "${target}" PARENT_SCOPE)
    set(${out_kind} "${kind}" PARENT_SCOPE)
    set(${out_tier} "${tier}" PARENT_SCOPE)
    set(${out_rel} "${rel}" PARENT_SCOPE)
endfunction()

# Cùng luật với dependency_error() trong tools/check_layers.py: CMake chỉ link theo đúng chiều.
function(_orion_check_dependency target kind tier dep)
    if(NOT TARGET ${dep})
        message(FATAL_ERROR
            "${target}: DEPS ${dep} không phải target; thư viện ngoài đặt ở EXTERNAL")
    endif()
    get_target_property(dep_kind ${dep} ORION_MODULE_KIND)
    get_target_property(dep_tier ${dep} ORION_MODULE_TIER)
    if(NOT dep_kind)
        message(FATAL_ERROR
            "${target}: ${dep} không phải module Orion; thư viện ngoài đặt ở EXTERNAL")
    endif()
    set(ok FALSE)
    if(kind STREQUAL "engine")
        if(dep_kind STREQUAL "engine" AND dep_tier LESS_EQUAL tier)
            set(ok TRUE)
        endif()
    elseif(dep_kind STREQUAL "engine")
        # T3, T4, server, bot chỉ thấy T0 và T1; client và tool thấy tới T2.
        if(dep_tier LESS_EQUAL 1 OR (dep_tier EQUAL 2 AND kind MATCHES "^(client|tool)$"))
            set(ok TRUE)
        endif()
    elseif(dep_kind STREQUAL "shared")
        set(ok TRUE)
    elseif(dep_kind STREQUAL "server_lib" AND kind MATCHES "^(server_lib|server|tool)$")
        set(ok TRUE)
    endif()
    if(NOT ok)
        message(FATAL_ERROR
            "${target} (${kind}, T${tier}) không được phụ thuộc "
            "${dep} (${dep_kind}, T${dep_tier}); xem ARCH §3")
    endif()
endfunction()

# Gom tệp nguồn của module: gốc, detail/, và thư mục nền tảng của hệ đích. game/shared gom thêm
# mọi thư mục con trừ tests/ (ARCH §2 chia nó theo mảng luật chơi).
function(_orion_module_sources dir kind out_sources out_headers)
    set(globs "${dir}/*.cpp" "${dir}/detail/*.cpp")
    set(header_globs "${dir}/*.hpp")
    if(kind STREQUAL "shared")
        file(GLOB children LIST_DIRECTORIES true CONFIGURE_DEPENDS "${dir}/*")
        foreach(child IN LISTS children)
            get_filename_component(name "${child}" NAME)
            if(IS_DIRECTORY "${child}" AND NOT name STREQUAL "tests" AND NOT name STREQUAL "detail")
                list(APPEND globs "${child}/*.cpp")
                list(APPEND header_globs "${child}/*.hpp")
            endif()
        endforeach()
    endif()
    foreach(platform IN LISTS ORION_PLATFORM_DIRS)
        list(APPEND globs "${dir}/${platform}/*.cpp")
        if(APPLE)
            list(APPEND globs "${dir}/${platform}/*.mm")
        endif()
    endforeach()
    file(GLOB sources CONFIGURE_DEPENDS ${globs})
    list(FILTER sources EXCLUDE REGEX "/main\\.cpp$")
    file(GLOB headers CONFIGURE_DEPENDS ${header_globs})
    set(${out_sources} "${sources}" PARENT_SCOPE)
    set(${out_headers} "${headers}" PARENT_SCOPE)
endfunction()

# Mỗi header public được biên dịch một mình trong một tệp sinh ra, để header nào thiếu include thì
# build đỏ ngay trong module của nó thay vì ở module dùng nó.
function(_orion_header_check target rel headers out_source)
    set(content "// Sinh bởi orion_add_module: mỗi header public phải tự biên dịch được.\n")
    foreach(header IN LISTS headers)
        file(RELATIVE_PATH header_rel "${PROJECT_SOURCE_DIR}" "${header}")
        string(APPEND content "#include \"${header_rel}\"\n")
    endforeach()
    set(path "${CMAKE_CURRENT_BINARY_DIR}/${target}_header_check.cpp")
    file(CONFIGURE OUTPUT "${path}" CONTENT "${content}")
    set(${out_source} "${path}" PARENT_SCOPE)
endfunction()

# orion_add_module([EXCEPTIONS] [DEPS <module>...] [EXTERNAL <imported>...]
#                  [TEST_DEPS <module>...] [TEST_EXTERNAL <imported>...])
#
# DEPS: module Orion, kiểm theo tầng, link PUBLIC. EXTERNAL: thư viện ngoài, link PRIVATE vì header
# public không được lộ kiểu của chúng (X.2). EXCEPTIONS: xem orion_apply_flags().
function(orion_add_module)
    cmake_parse_arguments(PARSE_ARGV 0 arg "EXCEPTIONS" ""
                          "DEPS;EXTERNAL;TEST_DEPS;TEST_EXTERNAL")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "orion_add_module: tham số lạ ${arg_UNPARSED_ARGUMENTS}")
    endif()
    _orion_module_identity("${CMAKE_CURRENT_SOURCE_DIR}" target kind tier rel)
    _orion_module_sources("${CMAKE_CURRENT_SOURCE_DIR}" ${kind} sources headers)
    _orion_header_check(${target} "${rel}" "${headers}" header_check)

    add_library(${target} STATIC ${sources} ${header_check})
    set(exceptions_flag "")
    if(arg_EXCEPTIONS)
        set(exceptions_flag EXCEPTIONS)
    endif()
    orion_apply_flags(${target} ${exceptions_flag})
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/gen")
    set_target_properties(${target} PROPERTIES
        ORION_MODULE_KIND "${kind}"
        ORION_MODULE_TIER "${tier}"
        ORION_MODULE_DIR "${rel}")
    foreach(dep IN LISTS arg_DEPS)
        _orion_check_dependency(${target} ${kind} ${tier} ${dep})
    endforeach()
    target_link_libraries(${target} PUBLIC ${arg_DEPS} PRIVATE ${arg_EXTERNAL})
    if(target STREQUAL "engine_core")
        # hiến pháp V.5: mọi target phụ thuộc cổng kiểm một cách gián tiếp qua engine_core.
        add_dependencies(engine_core orion_check_style)
    endif()

    _orion_module_tests(${target} "${rel}" "${exceptions_flag}"
                        "${arg_TEST_DEPS}" "${arg_TEST_EXTERNAL}")
endfunction()

# Tiện ích test dùng chung của engine/core (bộ đếm cấp phát): OBJECT library để operator new thay
# thế luôn được link vào, không bị linker bỏ qua như khi nằm trong thư viện tĩnh.
function(_orion_module_tests target rel exceptions_flag test_deps test_external)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    file(GLOB support CONFIGURE_DEPENDS "${dir}/support/*.cpp")
    if(support)
        add_library(${target}_testing OBJECT ${support})
        orion_apply_flags(${target}_testing ${exceptions_flag})
        target_link_libraries(${target}_testing PUBLIC ${target} GTest::gtest)
    endif()

    set(common_test_libs ${target} ${test_deps} ${test_external})
    if(TARGET engine_core_testing)
        list(APPEND common_test_libs engine_core_testing)
    endif()
    if(TARGET ${target}_testing AND NOT target STREQUAL "engine_core")
        list(APPEND common_test_libs ${target}_testing)
    endif()

    file(GLOB unit_tests CONFIGURE_DEPENDS "${dir}/*_test.cpp")
    if(unit_tests)
        add_executable(${target}_tests ${unit_tests})
        orion_apply_flags(${target}_tests ${exceptions_flag})
        target_link_libraries(${target}_tests PRIVATE
            ${common_test_libs} GTest::gtest GTest::gtest_main)
        if(NOT CMAKE_CROSSCOMPILING)
            # Liệt kê test lúc ctest chạy (PRE_TEST) từng quá 5 giây mặc định trên runner Windows
            # (CI run 36330676036, job windows ubsan: engine_core_tests hết giờ ở bước discover nên
            # cả ctest dừng). 60 giây vẫn dưới TIMEOUT của từng test.
            gtest_discover_tests(${target}_tests
                TEST_PREFIX "${rel}:"
                DISCOVERY_MODE PRE_TEST
                DISCOVERY_TIMEOUT 60
                PROPERTIES TIMEOUT 120)
        endif()
    endif()

    file(GLOB benches CONFIGURE_DEPENDS "${dir}/*_bench.cpp")
    if(benches)
        add_executable(${target}_bench ${benches})
        orion_apply_flags(${target}_bench ${exceptions_flag})
        target_link_libraries(${target}_bench PRIVATE
            ${common_test_libs} benchmark::benchmark benchmark::benchmark_main)
        if(NOT CMAKE_CROSSCOMPILING)
            # Một lượt ngắn để chắc benchmark còn chạy; số đo thật do job benchmark của CI lấy.
            add_test(NAME "${rel}:bench-smoke"
                     COMMAND ${target}_bench --benchmark_min_time=0.001s)
            set_tests_properties("${rel}:bench-smoke" PROPERTIES TIMEOUT 300)
        endif()
    endif()
endfunction()

# orion_add_fuzz_target(<tên> DEPS <module>...)
#
# Target fuzz_<tên> từ tests/fuzz/<tên>.cpp, corpus hạt giống ở tests/fuzz/corpus/<tên>/ (CLAUDE.md
# X.4). Khi ORION_FUZZ bật (preset linux-fuzz) target link libFuzzer và fuzz thật bằng
# tools/run_fuzz.py. Ở mọi preset khác nó link tests/fuzz/support/replay_main.cpp: không fuzz, chỉ
# chạy lại từng tệp corpus, để crash đã sửa không quay lại trên cả năm toolchain. ctest chạy lại
# corpus ở mọi preset, truyền thư mục chứ không truyền từng tệp: dòng lệnh Windows tối đa 32 767 ký
# tự, và corpus vài trăm tệp đã vượt.
function(orion_add_fuzz_target name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "DEPS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "orion_add_fuzz_target: tham số lạ ${arg_UNPARSED_ARGUMENTS}")
    endif()
    set(target fuzz_${name})
    set(corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
    file(GLOB corpus_files CONFIGURE_DEPENDS "${corpus}/*")
    if(NOT corpus_files)
        message(FATAL_ERROR "${target}: corpus hạt giống ${corpus} rỗng hoặc không có")
    endif()
    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp")
    orion_apply_flags(${target})
    target_link_libraries(${target} PRIVATE ${arg_DEPS})
    set(replay_args "${corpus}")
    if(ORION_FUZZ)
        # orion_apply_flags đã thêm fuzzer-no-link; cờ này link thêm main của libFuzzer.
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        # -runs=0: chạy mỗi input của thư mục một lần rồi thoát; không fuzz, không ghi vào thư mục.
        list(PREPEND replay_args -runs=0 "-artifact_prefix=${CMAKE_CURRENT_BINARY_DIR}/${name}-")
    else()
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests/fuzz/support/replay_main.cpp")
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME "tests/fuzz:${name}" COMMAND ${target} ${replay_args})
        set_tests_properties("tests/fuzz:${name}" PROPERTIES TIMEOUT 120)
    endif()
endfunction()

# orion_add_protocol(<target> NAMESPACE <namespace> HEADER <đường include> SCHEMAS <tệp>...)
#
# Sinh C++ từ các tệp *.schema bằng tools/codegen/orion_codegen.py (ADR 0004; docs/formats/
# protocol.md): header ở ${PROJECT_BINARY_DIR}/gen/<HEADER>, tệp .cpp cùng tên cạnh nó, rồi thêm cả
# hai vào <target>. Code sinh ra nằm trong out/ và không được commit (CLAUDE.md X.10); nó được sinh
# lại khi một schema hay một tệp của codegen đổi.
function(orion_add_protocol target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAMESPACE;HEADER" "SCHEMAS")
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_NAMESPACE OR NOT arg_HEADER OR NOT arg_SCHEMAS)
        message(FATAL_ERROR "orion_add_protocol: cần NAMESPACE, HEADER và ít nhất một SCHEMAS")
    endif()
    if(NOT arg_HEADER MATCHES "\\.hpp$")
        message(FATAL_ERROR "orion_add_protocol: HEADER ${arg_HEADER} phải là tệp .hpp")
    endif()
    set(header "${PROJECT_BINARY_DIR}/gen/${arg_HEADER}")
    string(REGEX REPLACE "\\.hpp$" ".cpp" source "${header}")
    file(GLOB codegen CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/codegen/*.py")
    # -B: không ghi __pycache__ vào cây nguồn mỗi lần build.
    add_custom_command(
        OUTPUT "${header}" "${source}"
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/codegen/orion_codegen.py"
                --namespace "${arg_NAMESPACE}" --header "${header}" --source "${source}"
                --include "${arg_HEADER}" --root "${PROJECT_SOURCE_DIR}" ${arg_SCHEMAS}
        DEPENDS ${arg_SCHEMAS} ${codegen}
        COMMENT "codegen: ${arg_HEADER}"
        VERBATIM)
    target_sources(${target} PRIVATE "${header}" "${source}")
endfunction()
