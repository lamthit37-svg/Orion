# Toolchain 4 của CLAUDE.md X.12: clang của NDK với libc++, arm64-v8a.
#
# Bọc toolchain của NDK và thêm cờ tất định, dùng chung cho code dự án (preset android-arm64) và
# port vcpkg (triplet arm64-android-orion). NDK lấy từ biến môi trường ANDROID_NDK_HOME; máy dev
# chưa có NDK (NGHI-NGO-007). Sàn API Android chưa chốt (ADR 0005), nên ANDROID_PLATFORM để NDK
# tự chọn mặc định cho tới khi ADR đó được chấp nhận.

if(NOT DEFINED ENV{ANDROID_NDK_HOME})
    message(FATAL_ERROR "android.cmake: đặt ANDROID_NDK_HOME trỏ tới thư mục NDK")
endif()
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI arm64-v8a)
endif()
set(ANDROID_STL c++_static)
include("$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")

# CLAUDE.md X.11: tắt hợp nhất FMA, để golden replay khớp với toolchain khác.
string(APPEND CMAKE_C_FLAGS_INIT " -ffp-contract=off")
string(APPEND CMAKE_CXX_FLAGS_INIT " -ffp-contract=off")
