// Benchmark của bitstream: ghi và đọc một bản cập nhật 100 thực thể, cỡ mà ngân sách mạng xuống của
// X.8 tính tới (100 thực thể trong tầm nhìn). Mỗi thực thể: id 16 bit, ba toạ độ lượng tử hoá 1 cm
// trong ±4096 m, hướng 0.01 rad, bốn cờ, máu 0..10000. Thêm ghi và đọc 1 KiB bytes, cỡ một mảnh
// tin cậy của docs/formats/channels.md; tham số là độ lệch bit của dữ liệu sau trường độ dài: 0 là
// thẳng hàng byte, 3 là lệch. Số đo ghi trong commit kèm preset và máy (X.8).

#include "engine/net/bitstream.hpp"

#include "engine/core/types.hpp"
#include "engine/math/random.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace orion::net {
namespace {

constexpr usize kEntities = 100;

struct Entity {
    i64 id = 0;
    std::array<f64, 3> position{};
    f64 yaw = 0;
    std::array<bool, 4> flags{};
    i64 health = 0;
};

const Quantization kPosition(-4096.0, 4096.0, 0.01);
const Quantization kYaw(0.0, 6.2832, 0.01);

[[nodiscard]] std::vector<Entity> make_entities() {
    math::Pcg32 random(99, 1);
    std::vector<Entity> entities(kEntities);
    for (Entity& entity : entities) {
        entity.id = random.next_below(65'536);
        for (f64& axis : entity.position) {
            axis = -4000.0 + (8000.0 * static_cast<f64>(random.next_u32()) / 4294967296.0);
        }
        entity.yaw = 6.28 * static_cast<f64>(random.next_u32()) / 4294967296.0;
        for (bool& flag : entity.flags) {
            flag = random.next_below(2) != 0;
        }
        entity.health = random.next_below(10'001);
    }
    return entities;
}

void encode(BitWriter& writer, const std::vector<Entity>& entities) {
    for (const Entity& entity : entities) {
        writer.write_ranged(entity.id, 0, 65'535);
        for (const f64 axis : entity.position) {
            writer.write_quantized(axis, kPosition);
        }
        writer.write_quantized(entity.yaw, kYaw);
        for (const bool flag : entity.flags) {
            writer.write_bool(flag);
        }
        writer.write_ranged(entity.health, 0, 10'000);
    }
}

void bitstream_write_100_entities(benchmark::State& state) {
    const std::vector<Entity> entities = make_entities();
    std::array<std::byte, 4096> buffer{};
    usize bytes = 0;
    for ([[maybe_unused]] auto iteration : state) {
        BitWriter writer(buffer);
        encode(writer, entities);
        bytes = writer.byte_count();
        benchmark::DoNotOptimize(buffer.data());
        benchmark::ClobberMemory();
    }
    state.counters["bytes"] = static_cast<f64>(bytes);
    state.SetItemsProcessed(state.iterations() * static_cast<i64>(kEntities));
}
BENCHMARK(bitstream_write_100_entities);

void bitstream_read_100_entities(benchmark::State& state) {
    const std::vector<Entity> entities = make_entities();
    std::array<std::byte, 4096> buffer{};
    BitWriter writer(buffer);
    encode(writer, entities);
    const std::span<const std::byte> message = writer.written();
    for ([[maybe_unused]] auto iteration : state) {
        BitReader reader(message);
        f64 checksum = 0;
        for (usize i = 0; i < kEntities; ++i) {
            checksum += static_cast<f64>(reader.read_ranged(0, 65'535).value_or(0));
            for (usize axis = 0; axis < 3; ++axis) {
                checksum += reader.read_quantized(kPosition).value_or(0.0);
            }
            checksum += reader.read_quantized(kYaw).value_or(0.0);
            for (usize flag = 0; flag < 4; ++flag) {
                checksum += reader.read_bool().value_or(false) ? 1.0 : 0.0;
            }
            checksum += static_cast<f64>(reader.read_ranged(0, 10'000).value_or(0));
        }
        benchmark::DoNotOptimize(checksum);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<i64>(kEntities));
}
BENCHMARK(bitstream_read_100_entities);

constexpr usize kFragment = 1024;
constexpr usize kMaxField = 1170;

// Số bit ghi trước trường độ dài (bits_for(kMaxField) = 11 bit) để dữ liệu lệch `shift` bit.
[[nodiscard]] u32 lead_bits(const i64 shift) {
    return static_cast<u32>((shift + 16 - static_cast<i64>(bits_for(kMaxField))) % 8);
}

[[nodiscard]] std::vector<std::byte> fragment() {
    std::vector<std::byte> bytes(kFragment);
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((i * 7U) & 0xFFU);
    }
    return bytes;
}

void bitstream_write_bytes_1k(benchmark::State& state) {
    const u32 offset = lead_bits(state.range(0));
    const std::vector<std::byte> bytes = fragment();
    std::array<std::byte, 1200> buffer{};
    for ([[maybe_unused]] auto iteration : state) {
        BitWriter writer(buffer);
        writer.write_bits(0, offset);
        writer.write_bytes(bytes, kMaxField);
        benchmark::DoNotOptimize(buffer.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<i64>(kFragment));
}
BENCHMARK(bitstream_write_bytes_1k)->Arg(0)->Arg(3);

void bitstream_read_bytes_1k(benchmark::State& state) {
    const u32 offset = lead_bits(state.range(0));
    std::array<std::byte, 1200> buffer{};
    BitWriter writer(buffer);
    writer.write_bits(0, offset);
    writer.write_bytes(fragment(), kMaxField);
    const std::span<const std::byte> message = writer.written();
    std::array<std::byte, kMaxField> out{};
    u64 total = 0;
    for ([[maybe_unused]] auto iteration : state) {
        BitReader reader(message);
        total += reader.read_bits(offset).value_or(0);
        total += reader.read_bytes(out, kMaxField).value_or(0);
        benchmark::DoNotOptimize(total);
        benchmark::DoNotOptimize(out.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<i64>(kFragment));
}
BENCHMARK(bitstream_read_bytes_1k)->Arg(0)->Arg(3);

}  // namespace
}  // namespace orion::net
