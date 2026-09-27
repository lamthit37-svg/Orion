# engine/math (T0): vector, quaternion, WorldPos, lượng giác tất định, PRNG (ARCH §2, §3).
orion_add_module(DEPS engine_core)

# Bảng giá trị làm tròn đúng cho test độ chính xác của trig; sinh bằng tools/gen_math_reference.py.
if(TARGET engine_math_tests)
    target_compile_definitions(engine_math_tests PRIVATE
        ORION_MATH_REFERENCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trig_reference.txt")
endif()
