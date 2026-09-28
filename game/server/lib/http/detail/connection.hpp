#pragma once

// Một kết nối HTTP/1.1 của server_http (nội bộ; docs/formats/http.md): chờ request, đọc header rồi
// body, đưa handler qua WorkerPool, ghi response, rồi chờ request kế tiếp hay đóng êm. Hạn của từng
// pha nằm trong deadline(); Server quét nó và gọi expire.
//
// Sở hữu: shared_ptr, vì kết nối phải sống tới khi thao tác Asio cuối cùng của nó xong (mỗi thao
// tác đang chờ giữ một bản), mà thứ tự các thao tác đó kết thúc do mạng quyết định (X.7). Registry
// chỉ giữ weak_ptr.
//
// Đồng bộ: mọi trường chỉ được chạm trên strand của kết nối (executor của socket_), trừ
// deadline_ns_ (atomic relaxed: việc quét hạn đọc nó từ luồng khác chỉ để chọn kết nối, rồi kiểm
// lại trên strand) và core_, id_, executor_ (không đổi sau khi dựng).

#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "game/server/lib/http/detail/core.hpp"
#include "game/server/lib/http/detail/message.hpp"
#include "game/server/lib/http/detail/response.hpp"
#include "game/server/lib/http/server.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <optional>

namespace orion::http::detail {

class Connection : public std::enable_shared_from_this<Connection> {
public:
    // `socket` phải có executor là một strand riêng của kết nối này.
    Connection(Core& core, boost::asio::ip::tcp::socket socket, u64 id);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;
    ~Connection() = default;

    // Ba hàm dưới gọi từ luồng nào cũng được: chúng chỉ đưa việc lên strand.
    // Bắt đầu chờ request đầu tiên.
    void start();
    // Hạn đã qua lúc `time`: đang đọc thì trả 408 rồi đóng; đang chờ, ghi hay đóng êm thì đóng.
    void expire(core::MonoTime time);
    // Đóng ngay, không response.
    void shutdown();

    // Hạn của pha hiện tại; không có hạn khi đang chờ handler. Gọi từ luồng nào cũng được.
    [[nodiscard]] core::MonoTime deadline() const noexcept;

    // Chạy trên một worker: gọi handler rồi đưa response về strand.
    void handle(const Message& message, core::MonoTime deadline) noexcept;

private:
    void wait_for_request();
    void on_first_bytes(const boost::system::error_code& error, usize bytes);
    void read_header();
    void on_header(const boost::system::error_code& error, usize bytes);
    void on_body(const boost::system::error_code& error);
    void fail_read(const boost::system::error_code& error);
    void dispatch();
    void respond(Response response);
    void on_write(const boost::system::error_code& error, bool keep_alive);
    void reject(Status status);
    void linger();
    void drain();
    void on_drain(const boost::system::error_code& error, usize bytes);
    void on_expire(core::MonoTime time);
    void close();

    void set_phase(Phase next) noexcept;
    void set_deadline(core::MonoTime deadline) noexcept;
    [[nodiscard]] core::MonoTime now() const noexcept;

    static constexpr usize kReadChunk = 4'096;

    Core* core_;  // không null, sống lâu hơn kết nối
    boost::asio::any_io_executor executor_;
    boost::asio::ip::tcp::socket socket_;
    boost::beast::flat_buffer buffer_;
    std::optional<RequestParser> parser_;
    BeastResponse response_;
    std::array<char, kReadChunk> drain_buffer_{};
    std::atomic<i64> deadline_ns_;
    usize drained_ = 0;
    u64 id_;
    u32 served_ = 0;
    Phase phase_ = Phase::New;
    // Của request đang xử lý.
    bool head_ = false;
    bool keep_alive_ = false;
    bool timed_out_ = false;
};

}  // namespace orion::http::detail
