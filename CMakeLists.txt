# Gốc build của Orion: chỉ khai dự án và add_subdirectory. Logic build nằm trong cmake/ (ARCH §2).
cmake_minimum_required(VERSION 4.4)
project(Orion VERSION 0.1.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(orion_flags)
include(orion_module)
enable_testing()

add_subdirectory(engine)
add_subdirectory(game)
# tools/ sau game/ vì tool dùng mọi tầng, kể cả game/server/lib (ARCH §3). Cổng kiểm
# orion_check_style khai trong tools/ vẫn chạy trước mọi target: engine_core phụ thuộc nó, và CMake
# nối phụ thuộc giữa các target lúc generate, sau khi đọc hết mọi thư mục.
add_subdirectory(tools)
add_subdirectory(tests)
