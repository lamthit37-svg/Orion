# engine/core (T0): chỉ phụ thuộc thư viện chuẩn và API hệ điều hành (ARCH §3).
orion_add_module()

# Runtime của TSan tự thay operator new để theo dõi bộ nhớ (libclang_rt.tsan_cxx), nên hook đếm cấp
# phát của test không link chung được; dưới TSan hook tắt và test đếm cấp phát tự bỏ qua.
if(ORION_SANITIZE STREQUAL "thread")
    target_compile_definitions(engine_core_testing PUBLIC ORION_ALLOC_HOOKS=0)
else()
    target_compile_definitions(engine_core_testing PUBLIC ORION_ALLOC_HOOKS=1)
endif()
