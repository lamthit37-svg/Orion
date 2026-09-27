# Triplet vcpkg của toolchain 5 (Apple clang, libc++): thư viện tĩnh, iOS arm64, build bằng đúng
# toolchain của code dự án (cmake/toolchains/ios.cmake).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/ios.cmake")
