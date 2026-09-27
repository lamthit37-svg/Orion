# Golden replay (CLAUDE.md X.4, X.11): mô phỏng của game/shared chạy trên các kịch bản input cố
# định, và hash trạng thái sau mỗi tick được so với golden/<kịch bản>.txt. CI chạy test này bằng
# MSVC, clang-cl, clang x64 và clang arm64, nên lệch một bit giữa các toolchain là test đỏ.
add_executable(replay_tests replay_test.cpp)
orion_apply_flags(replay_tests)
target_link_libraries(replay_tests PRIVATE game_shared GTest::gtest GTest::gtest_main)
target_compile_definitions(replay_tests PRIVATE
    ORION_REPLAY_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
if(NOT CMAKE_CROSSCOMPILING)
    gtest_discover_tests(replay_tests
        TEST_PREFIX "tests/replay:"
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT 120)
endif()
