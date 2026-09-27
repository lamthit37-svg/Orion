# Phần riêng của MMO (ARCH §2). Thứ tự add_subdirectory theo tầng ARCH §3.
add_subdirectory(shared)
# Server chạy trên Linux, và được build trên Windows để debug trên máy dev (ARCH §4.5). Android, iOS
# và macOS không build server, nên cũng không cài dependency của nó (vcpkg.json, ADR 0013).
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(server)
endif()
