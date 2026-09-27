# Toolchain 3 của CLAUDE.md X.12: clang trên Linux với libstdc++.
#
# Tệp này được dùng cho cả code dự án (VCPKG_CHAINLOAD_TOOLCHAIN_FILE của các preset linux*) lẫn mọi
# port vcpkg (triplet x64-linux-orion), để dependency và code dự án dùng cùng một compiler và cùng
# cờ tất định. Không dùng include_guard: CMake đọc lại toolchain cho từng try_compile.
#
# Chọn compiler: giữ CMAKE_CXX_COMPILER nếu đã đặt; nếu không thì lấy clang mới nhất có tên kèm số
# phiên bản, rồi mới tới `clang++` trần. Sàn phiên bản được kiểm trong orion_flags.cmake
# (NGHI-NGO-015).

if(NOT DEFINED CMAKE_CXX_COMPILER)
    foreach(_orion_version 22 21 20)
        find_program(_orion_clangxx NAMES clang++-${_orion_version} NO_CACHE)
        find_program(_orion_clang NAMES clang-${_orion_version} NO_CACHE)
        if(_orion_clangxx AND _orion_clang)
            break()
        endif()
        unset(_orion_clangxx)
        unset(_orion_clang)
    endforeach()
    if(NOT _orion_clangxx)
        find_program(_orion_clangxx NAMES clang++ NO_CACHE)
        find_program(_orion_clang NAMES clang NO_CACHE)
    endif()
    if(NOT _orion_clangxx OR NOT _orion_clang)
        message(FATAL_ERROR "linux-clang.cmake: không tìm thấy clang và clang++ (cần 20 trở lên)")
    endif()
    set(CMAKE_C_COMPILER "${_orion_clang}")
    set(CMAKE_CXX_COMPILER "${_orion_clangxx}")
endif()

# -fPIC: thư viện tĩnh được link vào tệp chạy PIE. -ffp-contract=off: CLAUDE.md X.11, NGHI-NGO-017.
# CMake đọc toolchain nhiều lần trong cùng một scope, nên chỉ thêm cờ chưa có.
foreach(_orion_flag -fPIC -ffp-contract=off)
    foreach(_orion_lang C CXX)
        if(NOT " ${CMAKE_${_orion_lang}_FLAGS_INIT} " MATCHES " ${_orion_flag} ")
            string(APPEND CMAKE_${_orion_lang}_FLAGS_INIT " ${_orion_flag}")
        endif()
    endforeach()
endforeach()
