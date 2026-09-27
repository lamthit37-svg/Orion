# Triplet vcpkg của toolchain 3 (clang trên Linux): thư viện tĩnh, build bằng đúng toolchain và cờ
# của code dự án (cmake/toolchains/linux-clang.cmake).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/linux-clang.cmake")
