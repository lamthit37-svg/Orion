# Target fuzz (CLAUDE.md X.4, X.16.9): mỗi parser nhận dữ liệu từ ngoài có một target ở đây, dựng
# bằng orion_add_fuzz_target(). Crash nào fuzz tìm ra thì input của nó vào corpus hạt giống, và
# thành một test unit của module có parser đó.
orion_add_fuzz_target(crypto_password_hash DEPS engine_crypto)
orion_add_fuzz_target(io_manifest DEPS engine_io engine_crypto)
orion_add_fuzz_target(io_pak_index DEPS engine_io)
orion_add_fuzz_target(net_address DEPS engine_net)
orion_add_fuzz_target(net_bitstream DEPS engine_net)
