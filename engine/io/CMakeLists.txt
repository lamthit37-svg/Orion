# engine/io (T1): tệp, pak, VFS, đọc bất đồng bộ (ARCH §2, §4.1). zstd chỉ được include ở đây
# (ADR 0012).
find_package(zstd CONFIG REQUIRED)
orion_add_module(DEPS engine_core engine_crypto engine_jobs EXTERNAL zstd::libzstd)
# Pak hạt giống của fuzz, do tools/gen_pak_seeds.py viết độc lập theo docs/formats/pak.md sinh ra;
# pak_seed_test.cpp đọc chúng.
if(TARGET engine_io_tests)
    target_compile_definitions(engine_io_tests PRIVATE
        ORION_PAK_SEEDS_DIR="${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/io_pak_index")
endif()
# Luật X.9 cho bản ship, kiểm ở mọi preset: tests/ship_api_check.cpp biên dịch với macro cấu hình
# của bản ship và không link vào đâu (CI không build preset ship).
if(TARGET engine_io_tests)
    add_library(engine_io_ship_api_check OBJECT tests/ship_api_check.cpp)
    orion_apply_flags(engine_io_ship_api_check SHIP)
    target_link_libraries(engine_io_ship_api_check PRIVATE engine_io)
endif()
