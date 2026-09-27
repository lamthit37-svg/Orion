#pragma once

// Đọc bất đồng bộ (ARCH §2, §7): một luồng IO tên "orion-io" đọc tệp qua Vfs cho luồng chính của
// client, để luồng chính không bao giờ chờ đĩa.
//
// - Đúng một luồng gọi submit và poll. Luồng IO xử lý yêu cầu theo đúng thứ tự gửi, nên kết quả về
//   theo thứ tự đó.
// - submit và poll không cấp phát và không chặn (X.7): yêu cầu và kết quả đi qua hai hàng đợi vòng
//   SPSC dành sẵn lúc start. Luồng IO cấp phát vector cho nội dung tệp.
// - Số yêu cầu đã gửi mà kết quả chưa được poll tối đa là capacity; submit trả false khi đã đủ, để
//   bên gọi gửi lại ở frame sau thay vì hàng đợi phình ra. Nhờ vậy hàng kết quả không bao giờ đầy.
// - Huỷ Streamer thì luồng IO dừng sau yêu cầu nó đang đọc; yêu cầu và kết quả còn lại bị bỏ.

#include "engine/core/error.hpp"
#include "engine/core/types.hpp"
#include "engine/io/path.hpp"
#include "engine/io/vfs.hpp"
#include "engine/jobs/thread.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace orion::io {

struct StreamRequest {
    // Do bên gọi đặt; kết quả mang lại đúng giá trị này.
    u64 ticket = 0;
    VirtualPath path;
};

struct StreamResult {
    u64 ticket = 0;
    // Như Vfs::read_all.
    Result<std::vector<std::byte>> content;
};

class Streamer {
public:
    // Tạo luồng IO đọc từ `vfs`, nhận tối đa `capacity` yêu cầu chưa poll (ít nhất 1). `vfs` phải
    // sống lâu hơn Streamer và không được mount thêm trong lúc Streamer chạy. Lỗi của
    // jobs::Thread::start.
    [[nodiscard]] static Result<Streamer> start(const Vfs& vfs, usize capacity);

    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;
    Streamer(Streamer&& other) noexcept;
    Streamer& operator=(Streamer&& other) noexcept;
    ~Streamer();

    // Gửi một yêu cầu đọc. false khi đã có capacity yêu cầu chưa poll.
    [[nodiscard]] bool submit(const StreamRequest& request) noexcept;
    // Chuyển tối đa out.size() kết quả đã xong vào đầu `out`; trả số kết quả.
    [[nodiscard]] usize poll(std::span<StreamResult> out) noexcept;
    // Số yêu cầu đã gửi mà kết quả chưa được poll.
    [[nodiscard]] usize pending() const noexcept { return pending_; }

private:
    struct State;

    Streamer(std::unique_ptr<State> state, jobs::Thread thread) noexcept;
    // Thân luồng IO.
    static void run(State& state, jobs::StopToken stop) noexcept;
    // Dừng và chờ luồng IO, rồi bỏ trạng thái.
    void stop() noexcept;

    std::unique_ptr<State> state_;
    jobs::Thread thread_;
    usize pending_ = 0;
};

}  // namespace orion::io
