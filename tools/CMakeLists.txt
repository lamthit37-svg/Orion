# Cổng kiểm (hiến pháp V.5, CLAUDE.md X.1). Target orion_check_style chạy cả ba script ở mỗi lần
# build; engine_core phụ thuộc nó, nên không target nào build được khi cổng đỏ.
add_custom_target(orion_check_style
    COMMAND "${Python3_EXECUTABLE}" tools/check_style.py
            --cache "${CMAKE_BINARY_DIR}/check_style.cache.json"
    COMMAND "${Python3_EXECUTABLE}" tools/check_layers.py
    COMMAND "${Python3_EXECUTABLE}" tools/check_tracked.py
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Cổng kiểm: check_style, check_layers, check_tracked"
    VERBATIM)

add_test(NAME tools.gates
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tools/tests -t tools
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
