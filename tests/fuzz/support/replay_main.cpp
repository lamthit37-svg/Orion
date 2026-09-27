// main thay libFuzzer ở các preset không bật ORION_FUZZ (CLAUDE.md X.4): mỗi tham số là một tệp
// corpus hay một thư mục corpus. Tệp được đưa vào LLVMFuzzerTestOneInput đúng một lần; thư mục thì
// từng tệp thường ngay trong nó (không đệ quy), theo thứ tự tên để lần chạy nào cũng như nhau.
// Không fuzz gì: chỉ để corpus, gồm cả input của các crash đã sửa, chạy lại trên cả năm toolchain.
//
// ctest truyền thư mục chứ không truyền từng tệp: dòng lệnh Windows tối đa 32 767 ký tự, và corpus
// vài trăm tệp đã vượt (CI run 36342991849).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path) {
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

// Các tệp của một tham số. Chỉ dùng các overload nhận error_code: build không có exception.
[[nodiscard]] std::optional<std::vector<fs::path>> inputs_of(const fs::path& argument) {
    std::error_code error;
    if (!fs::is_directory(argument, error)) {
        return std::vector<fs::path>{argument};
    }
    std::vector<fs::path> files;
    for (fs::directory_iterator it(argument, error); !error && it != fs::directory_iterator();
         it.increment(error)) {
        std::error_code type_error;
        if (it->is_regular_file(type_error)) {
            files.push_back(it->path());
        }
    }
    if (error) {
        return std::nullopt;
    }
    std::ranges::sort(files);
    return files;
}

void report(const std::string& message) {
    static_cast<void>(std::fputs(message.c_str(), stderr));
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) {
        report("cách dùng: <target> <tệp hay thư mục corpus>...\n");
        return 2;
    }
    std::size_t count = 0;
    for (const char* argument : args.subspan(1)) {
        const std::optional<std::vector<fs::path>> inputs = inputs_of(argument);
        if (!inputs.has_value()) {
            report(std::format("không liệt kê được {}\n", argument));
            return 1;
        }
        for (const fs::path& path : *inputs) {
            const std::optional<std::vector<std::uint8_t>> bytes = read_file(path);
            if (!bytes.has_value()) {
                report(std::format("không đọc được {}\n", path.string()));
                return 1;
            }
            LLVMFuzzerTestOneInput(bytes->data(), bytes->size());
            ++count;
        }
    }
    static_cast<void>(std::fputs(std::format("đã chạy lại {} input\n", count).c_str(), stdout));
    return 0;
}
