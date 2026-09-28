# tools/migrate (T5): orion_migrate, áp db/migrations/ vào PostgreSQL (CLAUDE.md X.14,
# docs/formats/migrations.md). Test dùng fixture PostgreSQL của server_db.
orion_add_module(DEPS engine_core engine_crypto server_db TEST_DEPS server_db_testing)
orion_add_program(orion_migrate)
if(TARGET tool_migrate_tests)
    target_compile_definitions(tool_migrate_tests PRIVATE
        ORION_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/db/migrations"
        ORION_MIGRATE_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch")
endif()
