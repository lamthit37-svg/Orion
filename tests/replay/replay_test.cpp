// Golden replay (CLAUDE.md X.4, X.11): mô phỏng của game/shared chạy trên các kịch bản input cố
// định, và hash trạng thái sau mỗi tick được so với tests/replay/golden/<kịch bản>.txt. Tệp golden
// được sinh một lần, rồi mọi toolchain CI chạy test này (MSVC, clang-cl, clang x64, clang arm64)
// phải cho đúng từng hash: lệch một bit ở bất kỳ tick nào là đỏ, và test in tick đầu tiên lệch.
//
// Input đi đúng đường của server: mỗi khung của mỗi nhân vật được mã hoá thành một MoveInput rồi
// giải mã lại (yaw vì vậy đã lượng tử hoá như trên dây), rồi thành ý định qua intent_from. Tệp
// golden ghi cả hash của chuỗi input, để khi lệch thì biết lệch do bộ sinh input hay do mô phỏng.
//
// Đổi mô phỏng có chủ đích thì sinh lại golden rồi commit, ghi lý do trong commit (X.4):
// `ORION_REPLAY_UPDATE=1 ctest --preset local -R tests/replay`.

#include "engine/core/types.hpp"
#include "engine/math/random.hpp"
#include "engine/math/scalar.hpp"
#include "engine/net/channels.hpp"
#include "game/shared/movement/movement.hpp"
#include "game/shared/protocol/codec.hpp"
#include "game/shared/protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace orion::replay {
namespace {

namespace fs = std::filesystem;

// FNV-1a 64 bit: hash so khớp, không cần chống giả mạo; mỗi từ vào theo little-endian.
class Fnv1a {
public:
    void add(const u64 word) noexcept {
        for (u32 byte = 0; byte < 8; ++byte) {
            value_ ^= (word >> (byte * 8U)) & 0xFFU;
            value_ *= 0x0000'0100'0000'01B3U;
        }
    }
    void add(const f64 number) noexcept { add(std::bit_cast<u64>(number)); }
    [[nodiscard]] u64 value() const noexcept { return value_; }

private:
    u64 value_ = 0xCBF2'9CE4'8422'2325U;
};

enum class Ground : u8 {
    Flat,
    // Bậc 1 m mỗi 20 m theo x (bậc lên và vách hụt), dốc 8% theo z, hố sâu 30 m bán kính 20 m ở
    // gốc.
    Terrain,
};

[[nodiscard]] f64 terrain(const f64 x, const f64 z) noexcept {
    const f64 height = math::floor(x / 20.0) + (0.08 * z);
    return (x * x) + (z * z) < 400.0 ? height - 30.0 : height;
}

struct Scenario {
    std::string_view name;
    u64 seed = 0;
    u32 entities = 0;
    u32 ticks = 0;
    // Vị trí đầu trong hình vuông cạnh 2·spread quanh (center_x, center_z).
    f64 center_x = 0.0;
    f64 center_z = 0.0;
    f64 spread = 0.0;
    Ground ground = Ground::Flat;
};

// gtest in tham số của test bằng hàm này (mặc định là dãy byte); gtest tìm đúng tên PrintTo.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const Scenario& scenario, std::ostream* out) {
    *out << scenario.name;
}

// Bộ sinh input: mỗi nhân vật giữ một ý định, đổi với xác suất 1/25 mỗi tick; nhảy 1/40.
class Inputs {
public:
    explicit Inputs(const Scenario& scenario)
        : random_(scenario.seed, 1), held_(scenario.entities) {}

    [[nodiscard]] protocol::MoveFrame next(const u32 entity) noexcept {
        protocol::MoveFrame& frame = held_[entity];
        if (random_.next_below(25) == 0) {
            frame.move_x = static_cast<i8>(static_cast<i32>(random_.next_below(201)) - 100);
            frame.move_z = static_cast<i8>(static_cast<i32>(random_.next_below(201)) - 100);
            frame.yaw = random_.next_f64() * 6.2832;
        }
        frame.jump = random_.next_below(40) == 0;
        return frame;
    }

private:
    math::Pcg32 random_;
    std::vector<protocol::MoveFrame> held_;
};

// Khung đi qua dây: mã hoá thành MoveInput, giải mã như server.
[[nodiscard]] protocol::MoveFrame through_wire(const protocol::MoveFrame& frame,
                                               const protocol::Tick tick) {
    protocol::MoveInput input{.tick = protocol::shorten(tick)};
    EXPECT_TRUE(input.frames.push_back(frame).has_value());
    std::array<std::byte, protocol::MoveInput::kMaxEncodedSize> buffer{};
    const usize size = protocol::encode(input, buffer).value_or(0);
    const auto decoded = protocol::decode_client_message(
        net::Channel::Sequenced, std::span<const std::byte>(buffer).first(size));
    EXPECT_TRUE(decoded.has_value());
    if (!decoded.has_value()) {
        return {};
    }
    return std::get<protocol::MoveInput>(*decoded).frames[0];
}

struct Trace {
    u64 input_hash = 0;
    std::vector<u64> tick_hashes;
};

[[nodiscard]] movement::State start_state(const Scenario& scenario, math::Pcg32& random) {
    const f64 x = scenario.center_x + (((random.next_f64() * 2.0) - 1.0) * scenario.spread);
    const f64 z = scenario.center_z + (((random.next_f64() * 2.0) - 1.0) * scenario.spread);
    movement::State state;
    state.position = {.x = math::clamp(x, -movement::kWorldHalfExtent, movement::kWorldHalfExtent),
                      .y = 0.0,
                      .z = math::clamp(z, -movement::kWorldHalfExtent, movement::kWorldHalfExtent)};
    return state;
}

[[nodiscard]] u64 hash_states(const std::vector<movement::State>& states) noexcept {
    Fnv1a hash;
    for (const movement::State& state : states) {
        hash.add(state.position.x);
        hash.add(state.position.y);
        hash.add(state.position.z);
        hash.add(state.yaw);
        hash.add(state.vertical_speed);
        hash.add(u64{state.grounded ? 1U : 0U});
    }
    return hash.value();
}

// flip_tick: tick mà input của nhân vật 0 bị đổi một đơn vị (test độ nhạy của hash).
[[nodiscard]] Trace simulate(const Scenario& scenario,
                             const std::optional<u32> flip_tick = std::nullopt) {
    math::Pcg32 placement(scenario.seed, 2);
    std::vector<movement::State> states;
    states.reserve(scenario.entities);
    for (u32 i = 0; i < scenario.entities; ++i) {
        states.push_back(start_state(scenario, placement));
    }
    const movement::Params params;
    const auto flat = [](const f64 /*x*/, const f64 /*z*/) noexcept {
        return 0.0;
    };
    Inputs inputs(scenario);
    Fnv1a input_hash;
    Trace run;
    for (u32 tick = 0; tick < scenario.ticks; ++tick) {
        for (u32 i = 0; i < scenario.entities; ++i) {
            protocol::MoveFrame frame = through_wire(inputs.next(i), protocol::Tick{tick});
            if (flip_tick == tick && i == 0) {
                frame.move_x = static_cast<i8>(frame.move_x == 100 ? 99 : frame.move_x + 1);
            }
            input_hash.add(std::bit_cast<u64>(frame.yaw));
            input_hash.add(u64{static_cast<u8>(frame.move_x)} |
                           (u64{static_cast<u8>(frame.move_z)} << 8U) |
                           (u64{frame.jump ? 1U : 0U} << 16U));
            const movement::Intent intent = movement::intent_from(frame);
            states[i] = scenario.ground == Ground::Flat
                            ? movement::step(states[i], intent, params, flat)
                            : movement::step(states[i], intent, params, terrain);
        }
        run.tick_hashes.push_back(hash_states(states));
    }
    run.input_hash = input_hash.value();
    return run;
}

// Kịch bản: đám đông trên mặt phẳng; trên địa hình có bậc, vách và hố; và dồn vào góc thế giới.
constexpr std::array kScenarios = {
    Scenario{.name = "crowd_flat", .seed = 1, .entities = 64, .ticks = 1'000, .spread = 200.0},
    Scenario{.name = "crowd_terrain",
             .seed = 2,
             .entities = 64,
             .ticks = 1'500,
             .spread = 150.0,
             .ground = Ground::Terrain},
    Scenario{.name = "world_edge",
             .seed = 3,
             .entities = 32,
             .ticks = 800,
             .center_x = 32'760.0,
             .center_z = -32'760.0,
             .spread = 10.0,
             .ground = Ground::Terrain},
};

// Thư mục golden trong cây nguồn, hay ORION_REPLAY_GOLDEN_DIR khi chạy ở nơi khác (thiết bị
// Android, máy Mac: NGHI-NGO-034).
[[nodiscard]] fs::path golden_path(const Scenario& scenario) {
    const char* dir = std::getenv("ORION_REPLAY_GOLDEN_DIR");  // NOLINT(concurrency-mt-unsafe)
    const fs::path base = dir != nullptr ? fs::path(dir) : fs::path(ORION_REPLAY_GOLDEN_DIR);
    return base / (std::string(scenario.name) + ".txt");
}

[[nodiscard]] std::string to_hex(const u64 value) {
    std::array<char, 16> digits{};
    const auto [end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    EXPECT_EQ(error, std::errc{});
    return std::string(16 - static_cast<usize>(end - digits.data()), '0') +
           std::string(digits.data(), end);
}

[[nodiscard]] std::optional<u64> from_hex(const std::string_view text) {
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size() || text.size() != 16) {
        return std::nullopt;
    }
    return value;
}

// Tệp golden: dòng comment `#`, rồi `input <hash>`, rồi mỗi tick một dòng `<tick> <hash>`.
void write_golden(const Scenario& scenario, const Trace& run) {
    std::ofstream out(golden_path(scenario), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out) << golden_path(scenario);
    out << "# Golden replay " << scenario.name
        << " (tests/replay/replay_test.cpp): hash FNV-1a của trạng thái mọi nhân vật\n"
        << "# sau mỗi tick. Sinh lại: ORION_REPLAY_UPDATE=1 ctest --preset local -R tests/replay\n"
        << "input " << to_hex(run.input_hash) << '\n';
    for (usize tick = 0; tick < run.tick_hashes.size(); ++tick) {
        out << tick << ' ' << to_hex(run.tick_hashes[tick]) << '\n';
    }
    ASSERT_TRUE(out.good());
}

[[nodiscard]] std::optional<Trace> read_golden(const Scenario& scenario) {
    std::ifstream in(golden_path(scenario), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Trace run;
    std::string line;
    bool have_input = false;
    while (std::getline(in, line)) {
        // .gitattributes giữ LF, nhưng tệp chép tay trên Windows có thể mang CRLF.
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const usize space = line.find(' ');
        if (space == std::string::npos) {
            return std::nullopt;
        }
        const std::string_view key(line.data(), space);
        const std::optional<u64> hash = from_hex(std::string_view(line).substr(space + 1));
        if (!hash.has_value()) {
            return std::nullopt;
        }
        if (key == "input") {
            run.input_hash = *hash;
            have_input = true;
        } else if (key == std::to_string(run.tick_hashes.size())) {
            run.tick_hashes.push_back(*hash);
        } else {
            return std::nullopt;
        }
    }
    return have_input ? std::optional<Trace>(run) : std::nullopt;
}

[[nodiscard]] bool update_requested() {
    const char* flag = std::getenv("ORION_REPLAY_UPDATE");  // NOLINT(concurrency-mt-unsafe)
    return flag != nullptr && std::string_view(flag) == "1";
}

class Replay : public ::testing::TestWithParam<Scenario> {};

TEST_P(Replay, MatchesTheGoldenHashOfEveryTick) {
    const Scenario& scenario = GetParam();
    const Trace run = simulate(scenario);
    ASSERT_EQ(run.tick_hashes.size(), scenario.ticks);
    if (update_requested()) {
        write_golden(scenario, run);
        GTEST_SKIP() << "đã ghi " << golden_path(scenario);
    }
    const std::optional<Trace> golden = read_golden(scenario);
    if (!golden.has_value()) {
        ADD_FAILURE() << "không đọc được " << golden_path(scenario)
                      << "; sinh bằng ORION_REPLAY_UPDATE=1";
        return;
    }
    ASSERT_EQ(run.input_hash, golden->input_hash)
        << "chuỗi input của " << scenario.name << " đã đổi: lệch do bộ sinh input hay protocol, "
        << "không phải do mô phỏng";
    ASSERT_EQ(golden->tick_hashes.size(), run.tick_hashes.size());
    for (usize tick = 0; tick < run.tick_hashes.size(); ++tick) {
        ASSERT_EQ(to_hex(run.tick_hashes[tick]), to_hex(golden->tick_hashes[tick]))
            << scenario.name << ": lệch đầu tiên ở tick " << tick;
    }
}

INSTANTIATE_TEST_SUITE_P(Scenarios, Replay, ::testing::ValuesIn(kScenarios),
                         [](const ::testing::TestParamInfo<Scenario>& info) {
                             return std::string(info.param.name);
                         });

// Hash phủ hết trạng thái: đổi một input của một nhân vật ở một tick thì từ tick đó hash khác, còn
// trước đó giữ nguyên. Chạy hai lần cho cùng kết quả (mô phỏng là hàm thuần).
TEST(ReplayHash, DetectsASingleChangedInput) {
    const Scenario& scenario = kScenarios[0];
    const Trace once = simulate(scenario);
    EXPECT_EQ(simulate(scenario).tick_hashes, once.tick_hashes);
    constexpr u32 kFlip = 300;
    const Trace flipped = simulate(scenario, kFlip);
    for (usize tick = 0; tick < once.tick_hashes.size(); ++tick) {
        if (tick < kFlip) {
            ASSERT_EQ(flipped.tick_hashes[tick], once.tick_hashes[tick]) << tick;
        } else {
            ASSERT_NE(flipped.tick_hashes[tick], once.tick_hashes[tick]) << tick;
        }
    }
}

}  // namespace
}  // namespace orion::replay
