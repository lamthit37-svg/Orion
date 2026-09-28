# game/server/lib/http (T4): server HTTP/1.1 và JSON của các dịch vụ HTTP (ARCH §4.6, §7; CLAUDE.md
# X.14). Boost.Beast, Boost.Asio (ADR 0015) và simdjson (ADR 0014) chỉ được include trong module
# này; test của module dùng Asio làm client qua loopback.
find_package(Boost 1.83 REQUIRED)
find_package(simdjson CONFIG REQUIRED)
orion_add_module(DEPS engine_core engine_jobs
                 EXTERNAL Boost::headers simdjson::simdjson
                 TEST_EXTERNAL Boost::headers)
if(WIN32)
    # Asio cần biết bản Windows tối thiểu (Windows 10), cho mọi tệp include nó, kể cả test; Winsock
    # 2 và mswsock cho socket của Asio.
    target_compile_definitions(server_http PUBLIC _WIN32_WINNT=0x0A00)
    target_link_libraries(server_http PRIVATE ws2_32 mswsock)
endif()
