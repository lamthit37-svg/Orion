# engine/io (T1): tệp, pak, VFS, đọc bất đồng bộ (ARCH §2, §4.1). zstd chỉ được include ở đây
# (ADR 0012).
find_package(zstd CONFIG REQUIRED)
orion_add_module(DEPS engine_core engine_crypto EXTERNAL zstd::libzstd TEST_DEPS engine_jobs)
# Pak hạt giống của fuzz, do tools/gen_pak_seeds.py viết độc lập theo docs/formats/pak.md sinh ra;
# pak_seed_test.cpp đọc chúng.
if(TARGET engine_io_tests)
    target_compile_definitions(engine_io_tests PRIVATE
        ORION_PAK_SEEDS_DIR="${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/io_pak_index")
endif()
