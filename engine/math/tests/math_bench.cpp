// Benchmark của engine/math: so hàm lượng giác tất định với bản của thư viện chuẩn, và đo các phép
// vector, quaternion, PRNG dùng trong mô phỏng. Số đo ghi trong commit kèm preset và máy (X.8).

#include "engine/core/types.hpp"
#include "engine/math/quat.hpp"
#include "engine/math/random.hpp"
#include "engine/math/trig.hpp"
#include "engine/math/vec.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cmath>

namespace orion::math {
namespace {

constexpr usize kInputCount = 1'024;

// Đầu vào dựng một lần ngoài vòng đo; vòng đo không cấp phát (X.7).
template <class T>
[[nodiscard]] std::array<T, kInputCount> make_inputs(const T lo, const T hi) {
    Pcg32 rng(0xBE7C, 1);
    std::array<T, kInputCount> values{};
    for (T& value : values) {
        value = lo + (hi - lo) * static_cast<T>(rng.next_f64());
    }
    return values;
}

template <class Fn>
void run_unary(benchmark::State& state, const std::array<f64, kInputCount>& inputs, Fn fn) {
    usize i = 0;
    for ([[maybe_unused]] auto iteration : state) {
        benchmark::DoNotOptimize(fn(inputs[i]));
        i = (i + 1) & (kInputCount - 1);
    }
}

void sin_orion(benchmark::State& state) {
    run_unary(state, make_inputs(-100.0, 100.0), [](f64 x) { return sin(x); });
}
BENCHMARK(sin_orion);

void sin_std(benchmark::State& state) {
    run_unary(state, make_inputs(-100.0, 100.0), [](f64 x) { return std::sin(x); });
}
BENCHMARK(sin_std);

void sincos_orion(benchmark::State& state) {
    run_unary(state, make_inputs(-100.0, 100.0), [](f64 x) { return sincos(x).cos; });
}
BENCHMARK(sincos_orion);

void atan_orion(benchmark::State& state) {
    run_unary(state, make_inputs(-10.0, 10.0), [](f64 x) { return atan(x); });
}
BENCHMARK(atan_orion);

void atan_std(benchmark::State& state) {
    run_unary(state, make_inputs(-10.0, 10.0), [](f64 x) { return std::atan(x); });
}
BENCHMARK(atan_std);

void atan2_orion(benchmark::State& state) {
    run_unary(state, make_inputs(-10.0, 10.0), [](f64 x) { return atan2(x, 1.5 - x); });
}
BENCHMARK(atan2_orion);

void atan2_std(benchmark::State& state) {
    run_unary(state, make_inputs(-10.0, 10.0), [](f64 x) { return std::atan2(x, 1.5 - x); });
}
BENCHMARK(atan2_std);

void pcg32_next_u32(benchmark::State& state) {
    Pcg32 rng(1, 1);
    for ([[maybe_unused]] auto iteration : state) {
        benchmark::DoNotOptimize(rng.next_u32());
    }
}
BENCHMARK(pcg32_next_u32);

void quat_rotate(benchmark::State& state) {
    const Quat q = from_axis_angle(normalize_or_zero(Vec3{1.0F, 2.0F, 3.0F}), 0.7F);
    Vec3 v{1.0F, 0.0F, 0.0F};
    for ([[maybe_unused]] auto iteration : state) {
        v = rotate(q, v);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(quat_rotate);

void vec3_normalize(benchmark::State& state) {
    const std::array<f32, kInputCount> inputs = make_inputs(-50.0F, 50.0F);
    usize i = 0;
    for ([[maybe_unused]] auto iteration : state) {
        const Vec3 v{inputs[i], inputs[(i + 1) & (kInputCount - 1)], 1.0F};
        benchmark::DoNotOptimize(normalize_or_zero(v));
        i = (i + 1) & (kInputCount - 1);
    }
}
BENCHMARK(vec3_normalize);

}  // namespace
}  // namespace orion::math
