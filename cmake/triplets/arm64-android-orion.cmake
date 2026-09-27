# Triplet vcpkg của toolchain 4 (clang NDK, libc++): thư viện tĩnh, arm64-v8a, build bằng đúng
# toolchain của code dự án (cmake/toolchains/android.cmake).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/android.cmake")
