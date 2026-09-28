# Target fuzz (CLAUDE.md X.4, X.16.9): mỗi parser nhận dữ liệu từ ngoài có một target ở đây, dựng
# bằng orion_add_fuzz_target(). Crash nào fuzz tìm ra thì input của nó vào corpus hạt giống, và
# thành một test unit của module có parser đó.
orion_add_fuzz_target(crypto_password_hash DEPS engine_crypto)
orion_add_fuzz_target(io_manifest DEPS engine_io engine_crypto)
orion_add_fuzz_target(io_pak_index DEPS engine_io)
orion_add_fuzz_target(net_address DEPS engine_net)
orion_add_fuzz_target(net_bitstream DEPS engine_net)
orion_add_fuzz_target(net_channels DEPS engine_net)
orion_add_fuzz_target(net_connect_token DEPS engine_net engine_crypto)
orion_add_fuzz_target(net_data_packet DEPS engine_net engine_crypto)
orion_add_fuzz_target(net_handshake DEPS engine_net engine_crypto)
orion_add_fuzz_target(net_transport DEPS engine_net engine_crypto)
orion_add_fuzz_target(protocol_codegen DEPS game_shared_test_protocol)
orion_add_fuzz_target(protocol_decode DEPS game_shared)
# server_db và server_http chỉ build cho Linux và Windows (game/CMakeLists.txt).
if(TARGET server_db)
    orion_add_fuzz_target(db_value DEPS server_db)
endif()
if(TARGET server_http)
    orion_add_fuzz_target(json_document DEPS server_http)
endif()
