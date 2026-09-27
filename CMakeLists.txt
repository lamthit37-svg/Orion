# Gốc build của Orion: chỉ khai dự án và add_subdirectory. Logic build nằm trong cmake/ (ARCH §2).
cmake_minimum_required(VERSION 4.4)
project(Orion VERSION 0.1.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(orion_flags)
include(orion_module)
enable_testing()

add_subdirectory(tools)
add_subdirectory(tests)
