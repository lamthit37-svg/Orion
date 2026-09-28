# game/server/lib/ledger (T4): ledger ghi kép của kinh tế (CLAUDE.md X.9, docs/formats/ledger.md).
# Test chạy trên PostgreSQL thật qua fixture của server_db; test đồng thời xin luồng qua
# engine/jobs (X.7).
orion_add_module(DEPS engine_core engine_crypto server_db
                 TEST_DEPS server_db_testing engine_jobs)
