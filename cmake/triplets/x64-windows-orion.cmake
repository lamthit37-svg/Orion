# Triplet vcpkg của toolchain 1 và 2 (MSVC, clang-cl): thư viện tĩnh, CRT động.
#
# CRT động để mọi DLL đi kèm game (D3D12 Agility SDK, FMOD) dùng chung một bản CRT với game.
# /fp:precise là mặc định của MSVC; ghi rõ để triplet không đổi theo mặc định (CLAUDE.md X.11).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_C_FLAGS "/fp:precise")
set(VCPKG_CXX_FLAGS "/fp:precise")
