// Benchmark của engine/io: đọc một entry pak (băm, giải nén), tìm đường dẫn trong index lớn, mở pak
// có kiểm hash index. Số đo ghi trong commit kèm preset và máy (X.8).

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/crypto/crypto.hpp"
#include "engine/io/detail/native_file.hpp"
#include "engine/io/file.hpp"
#include "engine/io/pak.hpp"
#include "engine/io/pak_builder.hpp"
#include "engine/io/path.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orion::io {
namespace {

// Pak tạm trong thư mục chạy benchmark; xoá khi ra khỏi phạm vi.
class BenchPak {
public:
    BenchPak(const std::string& name, const PakBuilder& builder)
        : path_("io_bench_" + name + ".pak") {
        const Result<PakSummary> written = builder.write(path_);
        if (written) {
            summary_ = *written;
        }
    }
    BenchPak(const BenchPak&) = delete;
    BenchPak& operator=(const BenchPak&) = delete;
    BenchPak(BenchPak&&) = delete;
    BenchPak& operator=(BenchPak&&) = delete;
    ~BenchPak() { static_cast<void>(detail::remove_file(path_.c_str()).has_value()); }

    [[nodiscard]] std::optional<PakReader> open() const {
        if (!summary_) {
            return std::nullopt;
        }
        Result<File> file = File::open(path_);
        if (!file) {
            return std::nullopt;
        }
        Result<PakReader> reader =
            PakReader::open(std::move(*file), summary_->file_size, summary_->index_hash);
        if (!reader) {
            return std::nullopt;
        }
        return std::move(*reader);
    }

private:
    std::string path_;
    std::optional<PakSummary> summary_;
};

[[nodiscard]] std::vector<std::byte> content_for(const bool compressible, const usize size) {
    std::vector<std::byte> bytes(size);
    u32 seed = 12'345;
    for (usize i = 0; i < size; ++i) {
        seed = (seed * 1'664'525U) + 1'013'904'223U;
        // Dữ liệu nén được: chữ lặp lại có biến thể nhỏ, gần với dữ liệu thiết kế đã biên dịch.
        bytes[i] = compressible
                       ? static_cast<std::byte>('a' + ((i / 7) % 13) + ((seed >> 30U) & 1U))
                       : static_cast<std::byte>(seed >> 24U);
    }
    return bytes;
}

// state.range(0): cỡ entry; state.range(1): 1 nếu nén được (Zstd), 0 nếu lưu thẳng.
void pak_read_entry(benchmark::State& state) {
    if (!crypto::initialize().has_value()) {
        state.SkipWithError("sodium_init thất bại");
        return;
    }
    const auto size = static_cast<usize>(state.range(0));
    const bool compressible = state.range(1) != 0;
    PakBuilder builder(19);
    const Result<VirtualPath> path = VirtualPath::parse("entry.bin");
    if (!path || !builder.add(*path, content_for(compressible, size)).has_value()) {
        state.SkipWithError("không dựng được pak");
        return;
    }
    const BenchPak pak(compressible ? "zstd" : "stored", builder);
    const std::optional<PakReader> reader = pak.open();
    if (!reader) {
        state.SkipWithError("không mở được pak");
        return;
    }
    PakReadContext context;
    context.reserve(reader->index().max_stored_size());
    std::vector<std::byte> out(size);
    for ([[maybe_unused]] auto iteration : state) {
        Result<usize> read = reader->read(0, out, context);
        benchmark::DoNotOptimize(read);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * state.range(0));
}
BENCHMARK(pak_read_entry)->Args({65'536, 0})->Args({65'536, 1})->Args({1'048'576, 1});

[[nodiscard]] std::string entry_name(const usize i) {
    return "textures/zone_" + std::to_string(i % 64) + "/tile_" + std::to_string(i) + ".ktx2";
}

// Index 10 000 entry: tìm đường dẫn có trong pak, và mở pak (đọc index, băm, phân tích).
void pak_index(benchmark::State& state, const bool open) {
    if (!crypto::initialize().has_value()) {
        state.SkipWithError("sodium_init thất bại");
        return;
    }
    PakBuilder builder(1);
    const std::vector<std::byte> tiny(16, std::byte{'t'});
    for (usize i = 0; i < 10'000; ++i) {
        const Result<VirtualPath> path = VirtualPath::parse(entry_name(i));
        if (!path || !builder.add(*path, tiny).has_value()) {
            state.SkipWithError("không dựng được pak");
            return;
        }
    }
    const BenchPak pak(open ? "open" : "find", builder);
    const std::optional<PakReader> reader = pak.open();
    if (!reader) {
        state.SkipWithError("không mở được pak");
        return;
    }
    usize next = 0;
    for ([[maybe_unused]] auto iteration : state) {
        if (open) {
            std::optional<PakReader> opened = pak.open();
            benchmark::DoNotOptimize(opened);
        } else {
            std::optional<usize> found = reader->index().find(entry_name(next));
            benchmark::DoNotOptimize(found);
            next = (next + 7'919) % 10'000;
        }
    }
}
BENCHMARK_CAPTURE(pak_index, find, false);
BENCHMARK_CAPTURE(pak_index, open, true)->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace orion::io
