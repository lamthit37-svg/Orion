# Triplet vcpkg cho preset asan: như x64-windows-orion, thêm _ANNOTATE_STL.
#
# Code dự án ở preset asan build với /fsanitize=address, nên STL của MSVC ghi dấu annotation
# container (detect_mismatch "annotate_string" = "1"); thư viện build không ASan ghi "0" và linker
# từ chối trộn hai loại (LNK2038, NGHI-NGO-025). _ANNOTATE_STL chèn sẵn code annotation mà không
# kích hoạt nó và không ghi dấu nào, nên thư viện link được với cả code có và không có ASan (xem
# __msvc_sanitizer_annotate_container.hpp của MSVC STL). Không bật /fsanitize=address cho
# dependency, vì toolchain Windows của vcpkg cứng /RTC1 ở Debug mà /RTC1 không đi cùng ASan.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_C_FLAGS "/fp:precise /D_ANNOTATE_STL")
set(VCPKG_CXX_FLAGS "/fp:precise /D_ANNOTATE_STL")
