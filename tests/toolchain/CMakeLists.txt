# Phần giao C++23 của năm toolchain (CLAUDE.md X.12). Target này biên dịch được nghĩa là mọi tính
# năng khai trong features.cpp có mặt trên toolchain đang build; CI build nó trên cả năm toolchain.
add_library(toolchain_features OBJECT features.cpp)
orion_apply_flags(toolchain_features)
add_dependencies(toolchain_features orion_check_style)
