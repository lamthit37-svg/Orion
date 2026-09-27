# engine/net (T1): socket, transport UDP, reliability, bitstream, mã hoá kênh (ARCH §2, §4.5;
# CLAUDE.md X.9, X.10; ADR 0004).
orion_add_module(DEPS engine_core engine_math)
if(WIN32)
    # Winsock 2 cho socket UDP của win/native_socket.cpp.
    target_link_libraries(engine_net PRIVATE ws2_32)
endif()
