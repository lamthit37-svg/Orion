# game/shared (T3): luật chơi thuần, dùng chung cho client và server (ARCH §2, §3; CLAUDE.md
# X.11). Nguồn nằm ở gốc và ở các thư mục con protocol/, movement/, combat/, defs/.
orion_add_module(DEPS engine_core engine_math engine_net)

# Protocol của game (ADR 0004): mọi tệp protocol/*.schema cùng làm thành một protocol, sinh thành
# game/shared/protocol/protocol.hpp trong out/ lúc build.
file(GLOB protocol_schemas CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.schema")
orion_add_protocol(game_shared
    NAMESPACE orion::protocol
    HEADER game/shared/protocol/protocol.hpp
    SCHEMAS ${protocol_schemas})

# Protocol thử của codegen (tests/protocol/*.schema): dùng mọi tính năng của ngôn ngữ schema, để
# code sinh ra được biên dịch và chạy trên cả năm toolchain. Chỉ link vào test và fuzz, không vào
# sản phẩm.
file(GLOB test_schemas CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol/*.schema")
add_library(game_shared_test_protocol STATIC)
orion_apply_flags(game_shared_test_protocol)
target_link_libraries(game_shared_test_protocol PUBLIC game_shared)
orion_add_protocol(game_shared_test_protocol
    NAMESPACE orion::protocol::testing
    HEADER game/shared/tests/generated/test_protocol.hpp
    SCHEMAS ${test_schemas})
target_link_libraries(game_shared_tests PRIVATE game_shared_test_protocol)
