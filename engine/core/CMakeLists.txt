# engine/core (T0): chỉ phụ thuộc thư viện chuẩn và API hệ điều hành (ARCH §3).
orion_add_module()

# Runtime của TSan tự thay operator new để theo dõi bộ nhớ (libclang_rt.tsan_cxx), nên hook đếm cấp
# phát của test không link chung được; dưới TSan hook tắt và test đếm cấp phát tự bỏ qua.
if(ORION_SANITIZE STREQUAL "thread")
    target_compile_definitions(engine_core_testing PUBLIC ORION_ALLOC_HOOKS=0)
else()
    target_compile_definitions(engine_core_testing PUBLIC ORION_ALLOC_HOOKS=1)
endif()

# environment_test.cpp: một lần chạy riêng có biến môi trường với giá trị không phải ASCII, để bản
# Windows của environment_variable (UTF-16 sang UTF-8) được kiểm bằng dữ liệu thật.
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME "engine/core:environment-utf8"
             COMMAND engine_core_tests --gtest_filter=Environment.ValuesAreUtf8)
    # Lần chạy này phải thật sự chạy và qua test đó: filter không khớp test nào (0 test, mã thoát 0)
    # hay test tự bỏ qua vì không thấy biến đều là đỏ, không phải xanh.
    set_tests_properties("engine/core:environment-utf8" PROPERTIES
        ENVIRONMENT "ORION_ENVIRONMENT_TEST=Hà Nội ☃"
        PASS_REGULAR_EXPRESSION "\\[       OK \\] Environment\\.ValuesAreUtf8"
        FAIL_REGULAR_EXPRESSION "SKIPPED;FAILED"
        TIMEOUT 60)
endif()
