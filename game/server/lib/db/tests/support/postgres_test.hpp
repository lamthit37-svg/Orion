#pragma once

// Fixture GoogleTest cho test cần PostgreSQL thật (CLAUDE.md X.4, ARCH §8 mục 3): server_db, rồi
// ledger và orion_migrate.
//
// - Chuỗi kết nối lấy từ biến môi trường ORION_TEST_POSTGRES. Không có thì test bỏ qua, trừ khi
//   ORION_REQUIRE_POSTGRES=1: khi đó test đỏ, để job CI có PostgreSQL không bao giờ xanh vì bỏ qua.
// - Mỗi test có một schema riêng, orion_test_<pid của backend>, đặt làm search_path của kết nối,
// nên
//   các tiến trình test chạy song song (ctest -j) không thấy bảng của nhau; schema bị xoá ở
//   TearDown, và một schema cùng tên còn sót lại từ lần chạy bị giết giữa chừng bị xoá ở SetUp.
// - Đồng hồ là đồng hồ giả không bao giờ chạy (X.4), nên mốc hạn deadline() không bao giờ tới và
//   không test nào phụ thuộc tốc độ máy. Test về hạn dùng đồng hồ riêng của nó.

#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "game/server/lib/db/db.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace orion::db::testing {

// Lỗi kèm thông điệp của server, cho thông điệp khi test đỏ: "Aborted: db: ... — <thông điệp>".
[[nodiscard]] std::string describe(const Error& error, const Connection& connection);

class PostgresTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    // Kết nối của test, đã ở trong schema riêng. Chỉ gọi sau SetUp thành công.
    [[nodiscard]] Connection& connection() noexcept;
    // Mốc hạn không bao giờ tới trên clock().
    [[nodiscard]] core::MonoTime deadline() const noexcept;
    [[nodiscard]] const core::FakeMonotonicClock& clock() const noexcept { return clock_; }
    [[nodiscard]] const std::string& conninfo() const noexcept { return conninfo_; }
    [[nodiscard]] const std::string& schema() const noexcept { return schema_; }

    // Một kết nối nữa vào cùng schema, đồng hồ `clock`: cho test hai phiên chạy xen nhau.
    [[nodiscard]] Result<Connection> connect_another(const core::MonotonicClock& clock) const;

private:
    core::FakeMonotonicClock clock_;
    std::optional<Connection> connection_;
    std::string conninfo_;
    std::string schema_;
};

}  // namespace orion::db::testing
