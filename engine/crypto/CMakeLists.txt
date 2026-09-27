# engine/crypto (T1): bọc libsodium (ARCH §4.1, ADR 0011). Nơi duy nhất include header của libsodium
# (X.2).
find_package(unofficial-sodium CONFIG REQUIRED)
orion_add_module(DEPS engine_core EXTERNAL unofficial-sodium::sodium)
