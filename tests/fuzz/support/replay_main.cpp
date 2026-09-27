// main thay libFuzzer ở các preset không bật ORION_FUZZ (CLAUDE.md X.4): mỗi tham số là một tệp
// corpus, được đưa vào LLVMFuzzerTestOneInput đúng một lần theo thứ tự truyền vào. Không fuzz gì:
// chỉ để corpus, gồm cả input của các crash đã sửa, chạy lại trên cả năm toolchain.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    const std::string content{std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>()};
    if (file.bad()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(content.size());
    std::ranges::transform(content, bytes.begin(),
                           [](const char c) { return static_cast<std::uint8_t>(c); });
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) {
        static_cast<void>(std::fputs("cách dùng: <target> <tệp corpus>...\n", stderr));
        return 2;
    }
    for (const char* path : args.subspan(1)) {
        const std::optional<std::vector<std::uint8_t>> bytes = read_file(path);
        if (!bytes.has_value()) {
            static_cast<void>(std::fputs(std::format("không đọc được {}\n", path).c_str(), stderr));
            return 1;
        }
        LLVMFuzzerTestOneInput(bytes->data(), bytes->size());
    }
    static_cast<void>(
        std::fputs(std::format("đã chạy lại {} input\n", args.size() - 1).c_str(), stdout));
    return 0;
}
