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

# Gom tệp nguồn của module: gốc, detail/, và thư mục nền tảng của hệ đích.
function(_orion_module_sources dir out_sources out_headers)
    set(globs "${dir}/*.cpp" "${dir}/detail/*.cpp")
    set(header_globs "${dir}/*.hpp")
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
    _orion_module_sources("${CMAKE_CURRENT_SOURCE_DIR}" sources headers)
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
            gtest_discover_tests(${target}_tests
                TEST_PREFIX "${rel}:"
                DISCOVERY_MODE PRE_TEST
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
