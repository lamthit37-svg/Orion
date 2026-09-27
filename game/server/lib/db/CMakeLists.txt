# game/server/lib/db (T4): bọc libpq (ARCH §4.5, §4.6; CLAUDE.md X.14; ADR 0013). libpq chỉ được
# include ở đây. find_package dùng module FindPostgreSQL của CMake; vcpkg bọc nó để thêm thư viện
# phụ của bản tĩnh (pgcommon, pgport, và secur32, wldap32 trên Windows).
find_package(PostgreSQL REQUIRED)
orion_add_module(DEPS engine_core EXTERNAL PostgreSQL::PostgreSQL)
if(WIN32)
    # Winsock 2 cho WSAPoll của win/socket_wait.cpp; libpq tĩnh cũng cần nó.
    target_link_libraries(server_db PRIVATE ws2_32)
endif()
