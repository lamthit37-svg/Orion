# game/server/lib/http (T4): HTTP và JSON của các dịch vụ HTTP (ARCH §4.6; CLAUDE.md X.14). simdjson
# (ADR 0014) chỉ được include trong module này.
find_package(simdjson CONFIG REQUIRED)
orion_add_module(DEPS engine_core EXTERNAL simdjson::simdjson)
