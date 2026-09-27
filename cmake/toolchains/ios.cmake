# Toolchain 5 của CLAUDE.md X.12: Apple clang với libc++, iOS arm64. Chỉ dùng trên Mac
# (NGHI-NGO-008), với generator Xcode (preset ios-arm64).
#
# Sàn iOS chưa chốt (ADR 0005, NGHI-NGO-022), nên CMAKE_OSX_DEPLOYMENT_TARGET để mặc định của SDK
# cho tới khi ADR đó được chấp nhận.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)

# CLAUDE.md X.11: tắt hợp nhất FMA, để golden replay khớp với toolchain khác.
string(APPEND CMAKE_C_FLAGS_INIT " -ffp-contract=off")
string(APPEND CMAKE_CXX_FLAGS_INIT " -ffp-contract=off")
