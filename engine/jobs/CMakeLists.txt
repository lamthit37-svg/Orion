# engine/jobs (T1): luồng có tên và job system work-stealing (ARCH §2, §4.1; ADR 0007). Nơi duy nhất
# tạo luồng hệ điều hành.
find_package(Threads REQUIRED)
orion_add_module(DEPS engine_core EXTERNAL Threads::Threads)
