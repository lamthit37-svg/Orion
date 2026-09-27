// Benchmark của di chuyển: một tick của cả zone ở tải của CLAUDE.md X.8, 500 người chơi và 5 000
// NPC, trên địa hình dốc để mọi nhánh bám đất, nhảy và rơi đều chạy. Ngân sách là cả tick zone p99
// ≤ 10 ms; di chuyển chỉ là một phần của nó. Số đo ghi trong commit kèm preset và máy (X.8).

#include "game/shared/movement/movement.hpp"

#include "engine/core/types.hpp"
#include "engine/math/random.hpp"

#include <benchmark/benchmark.h>

#include <vector>

namespace orion::movement {
namespace {

constexpr usize kEntities = 5'500;

void movement_step_zone(benchmark::State& bench) {
    math::Pcg32 random(2026, 9);
    std::vector<State> states(kEntities);
    std::vector<Intent> intents(kEntities);
    for (usize i = 0; i < kEntities; ++i) {
        states[i].position = {.x = (random.next_f64() * 2'000.0) - 1'000.0,
                              .y = 0.0,
                              .z = (random.next_f64() * 2'000.0) - 1'000.0};
        intents[i] = Intent{.x = (random.next_f64() * 1.4) - 0.7,
                            .z = (random.next_f64() * 1.4) - 0.7,
                            .yaw = random.next_f64() * 6.28,
                            .jump = random.next_below(16) == 0};
    }
    // Địa hình bậc thang: dốc 5% theo x, hụt 1 m mỗi 50 m theo z.
    const auto terrain = [](const f64 x, const f64 z) {
        return (0.05 * x) - static_cast<f64>(static_cast<i64>(z / 50.0));
    };
    const Params params;
    for ([[maybe_unused]] auto iteration : bench) {
        for (usize i = 0; i < kEntities; ++i) {
            states[i] = step(states[i], intents[i], params, terrain);
        }
        benchmark::DoNotOptimize(states.data());
        benchmark::ClobberMemory();
    }
    bench.SetItemsProcessed(static_cast<i64>(bench.iterations()) * static_cast<i64>(kEntities));
}
BENCHMARK(movement_step_zone);

}  // namespace
}  // namespace orion::movement
