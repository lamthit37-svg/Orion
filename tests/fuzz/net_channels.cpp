// Fuzz lớp kênh tin nhắn (CLAUDE.md X.4, X.9, X.10; docs/formats/channels.md): read_packet đọc dữ
// liệu do đầu kia của kết nối viết, nên là parser của dữ liệu từ mạng. Bit thấp của byte đầu chọn
// cách dùng phần còn lại:
//   0: dãy gói từ đầu kia. Byte kế chọn cỡ bộ đệm nhận tin cậy của đầu này; rồi mỗi gói là
//      2 byte độ dài (little-endian, lấy dư cho kMaxTransportPayload + 1), 1 byte bước thời gian
//      tính bằng ms, rồi dữ liệu. Hai bản MessageChannels giống hệt nhau (cùng cấu hình, cùng tin
//      nhắn đang gửi): A đọc mọi gói, B chỉ đọc gói A nhận. Gói bị từ chối phải trả lỗi trong bảng
//      Lỗi và không đổi gì: sau mỗi gói, A và B giao cùng tin nhắn, ghi ra cùng gói, cùng RTT và
//      cùng số tin nhắn đang bay.
//   1: kịch bản hai đầu nói chuyện qua mạng giả làm mất và đảo gói, đúng như transport để lại cho
//      lớp này (không nhân bản, không sửa gói). Mọi gói thật phải được nhận; tin nhắn giao ra phải
//      là tin nhắn đã gửi, không lần nào hai lần; ordered đúng thứ tự; sequenced không lùi. Hết
//      kịch bản thì chạy mạng không mất tới khi hết việc: mọi tin nhắn tin cậy phải tới, và không
//      còn gì chưa được xác nhận.
// ASan và UBSan của preset fuzz bắt phần còn lại.

#include "engine/core/assert.hpp"
#include "engine/core/bytes.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/channels.hpp"
#include "engine/net/connection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace {

using orion::u16;
using orion::u32;
using orion::u8;
using orion::usize;
namespace core = orion::core;
namespace net = orion::net;
using Bytes = std::vector<std::byte>;

constexpr core::MonoTime kStart = core::MonoTime::from_nanoseconds(1'000'000'000);
// Chế độ 0: cỡ bộ đệm nhận tin cậy của đầu kia, và các tin nhắn đầu này đang gửi.
constexpr usize kPeerBuffer = 8'192;
constexpr std::array<std::pair<net::Channel, usize>, 5> kPending = {{
    {net::Channel::Unreliable, 100},
    {net::Channel::Sequenced, net::kMaxUnreliableMessageSize},
    {net::Channel::ReliableOrdered, 3'000},
    {net::Channel::ReliableOrdered, 1},
    {net::Channel::ReliableUnordered, 2 * net::kFragmentSize},
}};
// Chế độ 1: số gói mỗi đầu ghi trong một nhịp của pha xả, và số nhịp tối đa của pha xả.
constexpr u32 kDrainPacketsPerTick = 8;
constexpr u32 kDrainTicks = 5'000;

// Đọc tuần tự input; hết byte thì mọi lần đọc trả 0 và rỗng.
class Script {
public:
    explicit Script(const std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool done() const noexcept { return bytes_.empty(); }
    [[nodiscard]] u8 byte() noexcept {
        if (bytes_.empty()) {
            return 0;
        }
        const auto value = std::to_integer<u8>(bytes_[0]);
        bytes_ = bytes_.subspan(1);
        return value;
    }
    [[nodiscard]] u16 u16_le() noexcept {
        const u8 low = byte();
        return static_cast<u16>(low | (u32{byte()} << 8U));
    }
    [[nodiscard]] std::span<const std::byte> take(const usize count) noexcept {
        const usize n = std::min(count, bytes_.size());
        const std::span<const std::byte> taken = bytes_.first(n);
        bytes_ = bytes_.subspan(n);
        return taken;
    }

private:
    std::span<const std::byte> bytes_;
};

[[nodiscard]] constexpr bool is_reliable(const net::Channel channel) noexcept {
    return channel == net::Channel::ReliableOrdered || channel == net::Channel::ReliableUnordered;
}

// Cỡ bộ đệm tin cậy từ một byte của input: 1 tới 130 561 byte, đủ cho tin nhắn 32 mảnh và vài tin
// nhắn lớn cùng bay.
[[nodiscard]] constexpr usize buffer_size_of(const u8 value) noexcept {
    return 1 + (usize{value} * 512);
}

// Cỡ tin nhắn tin cậy lớn nhất đi được vào bộ đệm nhận cỡ `buffer_size`.
[[nodiscard]] constexpr usize reliable_limit(const usize buffer_size) noexcept {
    return std::min(buffer_size, net::kMaxReliableMessageSize);
}

[[nodiscard]] net::MessageChannels make_channels(const usize receive_buffer,
                                                 const usize send_buffer) {
    orion::Result<net::MessageChannels> channels = net::MessageChannels::create(
        {.receive_buffer_size = receive_buffer, .send_buffer_size = send_buffer});
    ORION_VERIFY(channels.has_value(), "không tạo được MessageChannels của harness");
    return std::move(*channels);
}

// Tin nhắn số `serial` của harness: 4 byte đầu là serial (khi đủ dài), phần còn lại suy từ serial.
[[nodiscard]] Bytes message_of(const u32 serial, const usize size) {
    Bytes bytes(size);
    for (usize i = 0; i < size; ++i) {
        bytes[i] = i < 4 ? static_cast<std::byte>((serial >> (8 * i)) & 0xFFU)
                         : static_cast<std::byte>((usize{serial} * 131U) + (i * 7U));
    }
    return bytes;
}

// Tin nhắn nào cũng không rỗng và không vượt cỡ của kênh.
void check_messages(const std::span<const net::ReceivedMessage> messages,
                    const usize receive_buffer) {
    ORION_VERIFY(messages.size() <= net::kMaxMessagesPerPacket + net::kReliableWindow,
                 "một gói giao quá nhiều tin nhắn");
    for (const net::ReceivedMessage& message : messages) {
        const usize limit = is_reliable(message.channel) ? reliable_limit(receive_buffer)
                                                         : net::kMaxUnreliableMessageSize;
        ORION_VERIFY(std::to_underlying(message.channel) <= 3 && !message.data.empty() &&
                         message.data.size() <= limit,
                     "tin nhắn giao ra sai kênh hay sai cỡ");
    }
}

// Chế độ 0.
class Differential {
public:
    explicit Differential(const usize receive_buffer)
        : a_(make_channels(receive_buffer, kPeerBuffer)),
          b_(make_channels(receive_buffer, kPeerBuffer)),
          receive_buffer_(receive_buffer) {
        u32 serial = 0;
        for (const auto& [channel, size] : kPending) {
            const Bytes message = message_of(serial++, size);
            ORION_VERIFY(
                a_.send(channel, message).has_value() && b_.send(channel, message).has_value(),
                "không gửi được tin nhắn của harness");
        }
        compare_writes();
    }

    void packet(const std::span<const std::byte> bytes, const core::Duration step) {
        now_ = now_ + step;
        const orion::Result<std::span<const net::ReceivedMessage>> got_a =
            a_.read_packet(bytes, now_);
        if (got_a.has_value()) {
            check_messages(*got_a, receive_buffer_);
            const orion::Result<std::span<const net::ReceivedMessage>> got_b =
                b_.read_packet(bytes, now_);
            ORION_VERIFY(got_b.has_value() && same(*got_a, *got_b),
                         "gói bị từ chối trước đó đã đổi trạng thái");
        } else {
            const orion::ErrorCode code = got_a.error().code();
            ORION_VERIFY(
                code == orion::ErrorCode::OutOfRange || code == orion::ErrorCode::InvalidArgument,
                "read_packet trả mã lỗi ngoài bảng");
        }
        compare_writes();
    }

private:
    [[nodiscard]] static bool same(const std::span<const net::ReceivedMessage> x,
                                   const std::span<const net::ReceivedMessage> y) {
        return std::ranges::equal(x, y, [](const net::ReceivedMessage& m, const auto& n) {
            return m.channel == n.channel && std::ranges::equal(m.data, n.data);
        });
    }

    // A và B ghi ra đúng cùng các gói: xác nhận, gửi lại và RTT không phân biệt được.
    void compare_writes() {
        for (u32 i = 0; i < 4; ++i) {
            std::array<std::byte, net::kMaxTransportPayload> out_a{};
            std::array<std::byte, net::kMaxTransportPayload> out_b{};
            const usize size_a = a_.write_packet(out_a, now_);
            const usize size_b = b_.write_packet(out_b, now_);
            ORION_VERIFY(size_a == size_b && std::ranges::equal(std::span(out_a).first(size_a),
                                                                std::span(out_b).first(size_b)),
                         "gói bị từ chối trước đó đã đổi gói ghi ra");
            if (size_a == 0) {
                break;
            }
        }
        ORION_VERIFY(a_.rtt() == b_.rtt() &&
                         a_.in_flight(net::Channel::ReliableOrdered) ==
                             b_.in_flight(net::Channel::ReliableOrdered) &&
                         a_.in_flight(net::Channel::ReliableUnordered) ==
                             b_.in_flight(net::Channel::ReliableUnordered),
                     "gói bị từ chối trước đó đã đổi RTT hay tin nhắn đang bay");
    }

    net::MessageChannels a_;
    net::MessageChannels b_;
    usize receive_buffer_;
    core::MonoTime now_ = kStart;
};

void fuzz_packets(Script& script) {
    Differential differential(buffer_size_of(script.byte()));
    while (!script.done()) {
        const usize length = script.u16_le() % (net::kMaxTransportPayload + 1);
        const core::Duration step = core::Duration::milliseconds(script.byte());
        differential.packet(script.take(length), step);
    }
}

// Tin nhắn một đầu đã gửi mà đầu kia chưa nhận.
struct Outstanding {
    std::multiset<Bytes> unreliable;
    // Theo serial; serial tăng theo thứ tự gửi.
    std::map<u32, Bytes> sequenced;
    std::optional<u32> last_sequenced;
    std::vector<Bytes> ordered;
    usize ordered_delivered = 0;
    std::multiset<Bytes> unordered;

    void record(const net::Channel channel, const u32 serial, Bytes message) {
        switch (channel) {
            case net::Channel::Unreliable:
                unreliable.insert(std::move(message));
                return;
            case net::Channel::Sequenced:
                sequenced.emplace(serial, std::move(message));
                return;
            case net::Channel::ReliableOrdered:
                ordered.push_back(std::move(message));
                return;
            case net::Channel::ReliableUnordered:
                unordered.insert(std::move(message));
                return;
        }
    }

    // Tin nhắn đầu kia vừa nhận phải là một tin nhắn chưa nhận, theo luật của kênh.
    void deliver(const net::ReceivedMessage& message) {
        const Bytes data(message.data.begin(), message.data.end());
        switch (message.channel) {
            case net::Channel::Unreliable:
                take_one(unreliable, data);
                return;
            case net::Channel::Sequenced:
                deliver_sequenced(data);
                return;
            case net::Channel::ReliableOrdered:
                ORION_VERIFY(
                    ordered_delivered < ordered.size() && ordered[ordered_delivered] == data,
                    "ordered giao sai thứ tự, tin nhắn lạ hay giao hai lần");
                ++ordered_delivered;
                return;
            case net::Channel::ReliableUnordered:
                take_one(unordered, data);
                return;
        }
        ORION_VERIFY(false, "tin nhắn giao ra thuộc kênh lạ");
    }

    [[nodiscard]] bool reliable_done() const noexcept {
        return ordered_delivered == ordered.size() && unordered.empty();
    }

private:
    static void take_one(std::multiset<Bytes>& pending, const Bytes& data) {
        const auto found = pending.find(data);
        ORION_VERIFY(found != pending.end(), "tin nhắn lạ hay giao hai lần");
        pending.erase(found);
    }

    void deliver_sequenced(const Bytes& data) {
        ORION_VERIFY(data.size() >= 4, "tin nhắn sequenced lạ");
        const auto serial = core::load_le<u32>(std::span(data).first<4>());
        const auto found = sequenced.find(serial);
        ORION_VERIFY(found != sequenced.end() && found->second == data,
                     "tin nhắn sequenced lạ hay giao hai lần");
        ORION_VERIFY(!last_sequenced.has_value() || serial > *last_sequenced, "sequenced lùi");
        last_sequenced = serial;
        // Tin nhắn cũ hơn không bao giờ được giao nữa.
        sequenced.erase(sequenced.begin(), std::next(found));
    }
};

struct Endpoint {
    net::MessageChannels channels;
    usize receive_buffer = 0;
    // Tin nhắn đầu này đã gửi, chờ đầu kia nhận.
    Outstanding sent;
};

struct InFlight {
    core::MonoTime arrive;
    u32 order = 0;
    usize to = 0;
    Bytes bytes;
};

// Chế độ 1.
class Conversation {
public:
    Conversation(const usize buffer0, const usize buffer1)
        : ends_{Endpoint{.channels = make_channels(buffer0, buffer1),
                         .receive_buffer = buffer0,
                         .sent = {}},
                Endpoint{.channels = make_channels(buffer1, buffer0),
                         .receive_buffer = buffer1,
                         .sent = {}}} {}

    void step(Script& script) {
        switch (script.byte() % 5) {
            case 0:
                send(script);
                break;
            case 1:
                write(script);
                break;
            case 2:
                now_ = now_ + core::Duration::milliseconds(script.byte());
                deliver_due();
                break;
            case 3:
                if (!network_.empty()) {
                    deliver(script.byte() % network_.size());
                }
                break;
            default:
                if (!network_.empty()) {
                    network_.erase(network_.begin() +
                                   static_cast<std::ptrdiff_t>(script.byte() % network_.size()));
                }
                break;
        }
        for (const Endpoint& end : ends_) {
            ORION_VERIFY(
                end.channels.rtt() >= core::Duration{} &&
                    end.channels.in_flight(net::Channel::ReliableOrdered) <= net::kReliableWindow &&
                    end.channels.in_flight(net::Channel::ReliableUnordered) <= net::kReliableWindow,
                "RTT âm hay quá nhiều tin nhắn đang bay");
        }
    }

    // Mạng không mất, không trễ: mỗi nhịp mỗi đầu ghi tới kDrainPacketsPerTick gói, và mọi gói tới
    // ngay. Nhịp không ai ghi gì thì nhảy đúng một hạn gửi lại, max(50 ms, 1,5 × RTT) lớn nhất của
    // hai đầu: sau đó mọi mảnh chưa được xác nhận đều tới hạn, nên nhịp nào cũng có tiến triển.
    void drain() {
        bool busy = true;
        for (u32 tick = 0; tick < kDrainTicks && !finished(); ++tick) {
            now_ = now_ + (busy ? core::Duration::milliseconds(10) : resend_delay());
            busy = false;
            for (usize from = 0; from < 2; ++from) {
                for (u32 i = 0; i < kDrainPacketsPerTick; ++i) {
                    Bytes packet = write_one(from);
                    if (packet.empty()) {
                        break;
                    }
                    busy = true;
                    push(from, now_, std::move(packet));
                }
            }
            deliver_due();
        }
        ORION_VERIFY(finished(), "mạng không mất mà tin nhắn tin cậy vẫn chưa tới hết");
    }

private:
    void send(Script& script) {
        const u8 selector = script.byte();
        const usize from = selector & 1U;
        const auto channel = static_cast<net::Channel>((selector >> 1U) & 3U);
        const usize limit = is_reliable(channel) ? reliable_limit(ends_[1 - from].receive_buffer)
                                                 : net::kMaxUnreliableMessageSize;
        // Vài cỡ vượt giới hạn để thử nhánh lỗi; sequenced mang serial ở 4 byte đầu.
        usize size = 1 + (usize{script.u16_le()} % (limit + 8));
        if (channel == net::Channel::Sequenced) {
            size = std::max<usize>(size, 4);
        }
        const u32 serial = serial_++;
        Bytes message = message_of(serial, size);
        const orion::Result<void> sent = ends_[from].channels.send(channel, message);
        if (!sent.has_value()) {
            ORION_VERIFY(
                sent.error().code() == (size > limit ? orion::ErrorCode::InvalidArgument
                                                     : orion::ErrorCode::ResourceExhausted),
                "send hỏng sai lý do");
            return;
        }
        ORION_VERIFY(size <= limit, "send nhận tin nhắn quá cỡ");
        ends_[from].sent.record(channel, serial, std::move(message));
    }

    // Ghi tới 4 gói; mỗi gói mất với xác suất 1/4, hoặc tới sau 0 tới 63 ms.
    void write(Script& script) {
        const u8 selector = script.byte();
        const usize from = selector & 1U;
        const u32 count = 1 + ((selector >> 1U) & 3U);
        for (u32 i = 0; i < count; ++i) {
            Bytes packet = write_one(from);
            if (packet.empty()) {
                return;
            }
            const u8 fate = script.byte();
            if ((fate & 3U) != 0) {
                push(from, now_ + core::Duration::milliseconds(fate >> 2U), std::move(packet));
            }
        }
    }

    [[nodiscard]] Bytes write_one(const usize from) {
        std::array<std::byte, net::kMaxTransportPayload> out{};
        const usize size = ends_[from].channels.write_packet(out, now_);
        return {out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size)};
    }

    void push(const usize from, const core::MonoTime arrive, Bytes packet) {
        network_.push_back(
            {.arrive = arrive, .order = order_++, .to = 1 - from, .bytes = std::move(packet)});
    }

    void deliver_due() {
        std::ranges::sort(network_, [](const InFlight& x, const InFlight& y) {
            return x.arrive < y.arrive || (x.arrive == y.arrive && x.order < y.order);
        });
        usize due = 0;
        while (due < network_.size() && network_[due].arrive <= now_) {
            ++due;
        }
        const std::vector<InFlight> arrived(
            std::make_move_iterator(network_.begin()),
            std::make_move_iterator(network_.begin() + static_cast<std::ptrdiff_t>(due)));
        network_.erase(network_.begin(), network_.begin() + static_cast<std::ptrdiff_t>(due));
        for (const InFlight& packet : arrived) {
            receive(packet.to, packet.bytes);
        }
    }

    void deliver(const usize index) {
        const InFlight packet = std::move(network_[index]);
        network_.erase(network_.begin() + static_cast<std::ptrdiff_t>(index));
        receive(packet.to, packet.bytes);
    }

    void receive(const usize to, const Bytes& packet) {
        const orion::Result<std::span<const net::ReceivedMessage>> got =
            ends_[to].channels.read_packet(packet, now_);
        ORION_VERIFY(got.has_value(), "gói do MessageChannels ghi bị từ chối");
        check_messages(*got, ends_[to].receive_buffer);
        for (const net::ReceivedMessage& message : *got) {
            ends_[1 - to].sent.deliver(message);
        }
    }

    [[nodiscard]] core::Duration resend_delay() const noexcept {
        const core::Duration rtt = std::max(ends_[0].channels.rtt(), ends_[1].channels.rtt());
        return std::max(net::kMinResendDelay, (rtt * 3) / 2);
    }

    [[nodiscard]] bool finished() const noexcept {
        return network_.empty() && std::ranges::all_of(ends_, [](const Endpoint& end) {
                   return end.sent.reliable_done() &&
                          end.channels.in_flight(net::Channel::ReliableOrdered) == 0 &&
                          end.channels.in_flight(net::Channel::ReliableUnordered) == 0;
               });
    }

    std::array<Endpoint, 2> ends_;
    std::vector<InFlight> network_;
    u32 order_ = 0;
    u32 serial_ = 0;
    core::MonoTime now_ = kStart;
};

void fuzz_conversation(Script& script) {
    const usize buffer0 = buffer_size_of(script.byte());
    const usize buffer1 = buffer_size_of(script.byte());
    Conversation conversation(buffer0, buffer1);
    while (!script.done()) {
        conversation.step(script);
    }
    conversation.drain();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    // Chép sang std::byte thay vì ép kiểu con trỏ (X.3).
    std::vector<std::byte> input(size - 1);
    if (!input.empty()) {
        std::memcpy(input.data(), data + 1, input.size());
    }
    Script script(input);
    if ((data[0] & 1U) != 0) {
        fuzz_conversation(script);
    } else {
        fuzz_packets(script);
    }
    return 0;
}
