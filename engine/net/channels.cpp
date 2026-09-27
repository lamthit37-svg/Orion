#include "engine/net/channels.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/error.hpp"
#include "engine/core/time.hpp"
#include "engine/core/types.hpp"
#include "engine/net/bitstream.hpp"
#include "engine/net/connection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace orion::net {
namespace {

// Bố cục của gói (docs/formats/channels.md, mục Gói).
constexpr u32 kSequenceBits = 16;
constexpr u32 kAckBitsCount = 32;
constexpr u32 kChannelBits = 2;
constexpr u32 kOffsetBits = bits_for(kMaxReliableBufferSize - 1);
constexpr usize kMaxDataField = kMaxTransportPayload;
constexpr usize kUnreliableQueueRecords = kMaxMessagesPerPacket;
constexpr u16 kHalfRange = 0x8000;

[[nodiscard]] constexpr u16 add16(const u16 value, const u32 delta) noexcept {
    return static_cast<u16>(value + delta);
}
// (a − b) mod 2^16.
[[nodiscard]] constexpr u16 diff16(const u16 a, const u16 b) noexcept {
    return static_cast<u16>(a - b);
}
// a mới hơn b theo vòng.
[[nodiscard]] constexpr bool newer16(const u16 a, const u16 b) noexcept {
    const u16 d = diff16(a, b);
    return d != 0 && d < kHalfRange;
}

[[nodiscard]] constexpr bool is_reliable(const Channel channel) noexcept {
    return channel == Channel::ReliableOrdered || channel == Channel::ReliableUnordered;
}
[[nodiscard]] constexpr usize reliable_index(const Channel channel) noexcept {
    return channel == Channel::ReliableOrdered ? 0 : 1;
}

// Số bit của một tin nhắn trên dây, gồm cả bit cờ đứng trước nó.
[[nodiscard]] constexpr usize message_bits(const Channel channel, const usize data_size) noexcept {
    usize bits = 1 + kChannelBits + bits_for(kMaxDataField) + (8 * data_size);
    if (channel == Channel::Sequenced) {
        bits += kSequenceBits;
    } else if (is_reliable(channel)) {
        bits += kSequenceBits + (2 * bits_for(kMaxFragments - 1)) + kOffsetBits;
    }
    return bits;
}

[[nodiscard]] constexpr u32 fragment_count_of(const usize size) noexcept {
    return static_cast<u32>((size + kFragmentSize - 1) / kFragmentSize);
}

[[nodiscard]] constexpr u32 all_fragments(const u32 count) noexcept {
    return count == 32 ? ~u32{0} : ((u32{1} << count) - 1);
}

struct SendSlot {
    bool used = false;
    u16 id = 0;
    u32 fragment_count = 0;
    // Đoạn [offset, offset + size) của vòng đệm gửi, cũng là của bộ đệm nhận bên kia.
    usize offset = 0;
    usize size = 0;
    u32 acked = 0;
    u32 sent = 0;
    std::array<core::MonoTime, kMaxFragments> last_sent{};
};

struct SendChannel {
    std::vector<SendSlot> slots;
    // Vòng đệm FIFO: tin nhắn trong cửa sổ nằm liền nhau theo thứ tự id, có thể quay về đầu một
    // lần.
    std::vector<std::byte> ring;
    u16 next_id = 0;
    // Id nhỏ nhất chưa được xác nhận hết; cửa sổ là [oldest, next_id).
    u16 oldest = 0;

    // Chỗ liền cho `size` byte (size <= ring.size(), send đã kiểm) sau tin nhắn mới nhất của cửa
    // sổ. Đoạn của một tin nhắn chỉ được dùng lại khi oldest đã qua nó, nên chỗ trống là phần ngoài
    // các đoạn từ oldest tới tin nhắn mới nhất.
    [[nodiscard]] std::optional<usize> allocate(const usize size) const noexcept {
        if (oldest == next_id) {
            return 0;  // Cửa sổ rỗng: cả bộ đệm trống.
        }
        const usize tail = slots[oldest % kReliableWindow].offset;
        const SendSlot& newest = slots[static_cast<u16>(next_id - 1U) % kReliableWindow];
        const usize head = newest.offset + newest.size;
        if (newest.offset >= tail) {
            // Chưa quay vòng: [tail, head) đang dùng, trống [head, cuối) và [0, tail).
            if (ring.size() - head >= size) {
                return head;
            }
            return size <= tail ? std::optional<usize>(0) : std::nullopt;
        }
        // Đã quay vòng: chỉ [head, tail) trống.
        return tail - head >= size ? std::optional<usize>(head) : std::nullopt;
    }
};

enum class SlotState : u8 {
    Free,
    Receiving,
    Complete,
    Delivered,
};

struct ReceiveSlot {
    SlotState state = SlotState::Free;
    u16 id = 0;
    u32 fragment_count = 0;
    u32 received = 0;
    // Đoạn bên gửi đã chọn trong bộ đệm nhận; size biết khi có mảnh cuối.
    usize offset = 0;
    usize size = 0;
};

struct ReceiveChannel {
    std::vector<ReceiveSlot> slots;
    std::vector<std::byte> buffer;
    // Id nhỏ nhất chưa giao; cửa sổ nhận là [first_undelivered, first_undelivered + 64).
    u16 first_undelivered = 0;
};

struct FragmentRef {
    u8 channel = 0;
    u8 fragment = 0;
    u16 id = 0;
};

struct SentPacket {
    bool valid = false;
    bool acked = false;
    u16 sequence = 0;
    core::MonoTime sent_at;
    u32 count = 0;
    std::array<FragmentRef, kMaxMessagesPerPacket> fragments{};
};

struct QueuedMessage {
    Channel channel = Channel::Unreliable;
    u16 sequence = 0;
    usize offset = 0;
    usize size = 0;
};

struct ParsedMessage {
    Channel channel = Channel::Unreliable;
    // Số thứ tự (sequenced) hay id (tin cậy).
    u16 number = 0;
    u32 fragment_count = 1;
    u32 fragment_index = 0;
    usize offset = 0;
    // Tin nhắn tin cậy đã giao từ trước: bỏ qua khi áp.
    bool duplicate = false;
    std::span<const std::byte> data;
};

struct ParsedPacket {
    u16 sequence = 0;
    bool has_ack = false;
    u16 ack = 0;
    u32 ack_bits = 0;
    u32 count = 0;
    bool has_reliable = false;
    std::array<ParsedMessage, kMaxMessagesPerPacket> messages{};
};

}  // namespace

struct MessageChannels::State {
    explicit State(const ChannelsConfig& config)
        : queue_bytes(kUnreliableQueueBytes), queue(kUnreliableQueueRecords) {
        for (SendChannel& channel : send) {
            channel.slots.resize(kReliableWindow);
            channel.ring.resize(config.send_buffer_size);
        }
        for (ReceiveChannel& channel : receive) {
            channel.slots.resize(kReliableWindow);
            channel.buffer.resize(config.receive_buffer_size);
        }
    }

    [[nodiscard]] core::Duration resend_delay() const noexcept {
        return std::max(kMinResendDelay, (rtt * 3) / 2);
    }

    [[nodiscard]] Result<void> queue_unreliable(Channel channel,
                                                std::span<const std::byte> message) noexcept;
    [[nodiscard]] Result<void> queue_reliable(Channel channel,
                                              std::span<const std::byte> message) noexcept;

    // Thêm các tin nhắn unreliable đầu hàng còn vừa; trả số đã thêm.
    [[nodiscard]] usize write_unreliable(BitWriter& writer, usize& remaining,
                                         u32& written) noexcept;
    void write_reliable(BitWriter& writer, usize& remaining, u32& written, SentPacket& record,
                        core::MonoTime now) noexcept;
    void drop_queued(usize count) noexcept;

    [[nodiscard]] Result<void> parse(std::span<const std::byte> packet) noexcept;
    [[nodiscard]] Result<void> parse_message(BitReader& reader, ParsedMessage& message,
                                             usize& scratch_used) noexcept;
    [[nodiscard]] Result<void> check_reliable(ParsedMessage& message) const noexcept;
    void release_delivered() noexcept;
    void record_received(u16 sequence) noexcept;
    void apply_acks(core::MonoTime now) noexcept;
    void acknowledge(u16 sequence, core::MonoTime now) noexcept;
    void apply_message(const ParsedMessage& message) noexcept;
    void store_fragment(const ParsedMessage& message) noexcept;
    void deliver(Channel channel, std::span<const std::byte> data) noexcept;
    void release_ordered() noexcept;

    std::array<SendChannel, 2> send;
    std::array<ReceiveChannel, 2> receive;

    u16 next_packet_sequence = 0;
    std::array<SentPacket, kSentPacketHistory> sent{};
    bool has_received = false;
    u16 received_highest = 0;
    u32 received_bits = 0;
    bool ack_pending = false;
    bool has_sent = false;
    core::MonoTime last_packet_sent;
    core::Duration rtt = kInitialRtt;

    std::vector<std::byte> queue_bytes;
    usize queue_used = 0;
    std::vector<QueuedMessage> queue;
    usize queue_count = 0;
    u16 next_sequenced = 0;
    bool has_sequenced = false;
    u16 last_sequenced = 0;

    // Dữ liệu các tin nhắn của gói đang đọc. read_bytes cần chỗ cho cỡ tối đa ở mỗi lần đọc; tổng
    // dữ liệu một gói không vượt cỡ gói, nên gấp đôi là luôn đủ.
    std::array<std::byte, 2 * kMaxTransportPayload> parse_scratch{};
    ParsedPacket parsed;
    std::array<ReceivedMessage, kMaxMessagesPerPacket + kReliableWindow> delivered{};
    usize delivered_count = 0;
};

Result<void> MessageChannels::State::queue_unreliable(
    const Channel channel, const std::span<const std::byte> message) noexcept {
    if (queue_count == queue.size() || queue_used + message.size() > queue_bytes.size()) {
        return fail(ErrorCode::ResourceExhausted, "net: hàng tin nhắn unreliable đầy");
    }
    QueuedMessage& queued = queue[queue_count++];
    queued.channel = channel;
    queued.offset = queue_used;
    queued.size = message.size();
    queued.sequence = 0;
    if (channel == Channel::Sequenced) {
        queued.sequence = next_sequenced;
        next_sequenced = add16(next_sequenced, 1);
    }
    std::ranges::copy(message, queue_bytes.begin() + static_cast<std::ptrdiff_t>(queue_used));
    queue_used += message.size();
    return {};
}

Result<void> MessageChannels::State::queue_reliable(
    const Channel channel, const std::span<const std::byte> message) noexcept {
    SendChannel& out = send[reliable_index(channel)];
    if (message.size() > std::min(out.ring.size(), kMaxReliableMessageSize)) {
        return fail(ErrorCode::InvalidArgument, "net: tin nhắn lớn hơn bên kia nhận",
                    static_cast<i64>(message.size()));
    }
    if (diff16(out.next_id, out.oldest) >= kReliableWindow) {
        return fail(ErrorCode::ResourceExhausted, "net: cửa sổ tin cậy đầy");
    }
    const std::optional<usize> offset = out.allocate(message.size());
    if (!offset) {
        return fail(ErrorCode::ResourceExhausted, "net: bộ đệm gửi tin cậy hết chỗ");
    }
    SendSlot& slot = out.slots[out.next_id % kReliableWindow];
    ORION_VERIFY(!slot.used, "net: ô gửi của id mới chưa được giải phóng");
    slot = SendSlot{};
    slot.used = true;
    slot.id = out.next_id;
    slot.offset = *offset;
    slot.size = message.size();
    slot.fragment_count = fragment_count_of(message.size());
    std::ranges::copy(message, out.ring.begin() + static_cast<std::ptrdiff_t>(slot.offset));
    out.next_id = add16(out.next_id, 1);
    return {};
}

usize MessageChannels::State::write_unreliable(BitWriter& writer, usize& remaining,
                                               u32& written) noexcept {
    usize taken = 0;
    while (taken < queue_count && written < kMaxMessagesPerPacket) {
        const QueuedMessage& queued = queue[taken];
        const usize bits = message_bits(queued.channel, queued.size);
        if (bits > remaining) {
            break;  // Giữ thứ tự: tin nhắn sau đợi gói sau.
        }
        writer.write_bool(true);
        writer.write_bits(std::to_underlying(queued.channel), kChannelBits);
        if (queued.channel == Channel::Sequenced) {
            writer.write_bits(queued.sequence, kSequenceBits);
        }
        writer.write_bytes(std::span(queue_bytes).subspan(queued.offset, queued.size),
                           kMaxDataField);
        remaining -= bits;
        ++written;
        ++taken;
    }
    return taken;
}

void MessageChannels::State::write_reliable(BitWriter& writer, usize& remaining, u32& written,
                                            SentPacket& record, const core::MonoTime now) noexcept {
    const core::Duration delay = resend_delay();
    for (const Channel channel : {Channel::ReliableOrdered, Channel::ReliableUnordered}) {
        SendChannel& out = send[reliable_index(channel)];
        const u16 span = diff16(out.next_id, out.oldest);
        for (u16 k = 0; k < span && written < kMaxMessagesPerPacket; ++k) {
            const u16 id = add16(out.oldest, k);
            SendSlot& slot = out.slots[id % kReliableWindow];
            if (!slot.used || slot.id != id) {
                continue;
            }
            for (u32 f = 0; f < slot.fragment_count && written < kMaxMessagesPerPacket; ++f) {
                const u32 bit = u32{1} << f;
                const bool due = (slot.sent & bit) == 0 || now - slot.last_sent[f] >= delay;
                if ((slot.acked & bit) != 0 || !due) {
                    continue;
                }
                const usize start = usize{f} * kFragmentSize;
                const usize size = std::min(kFragmentSize, slot.size - start);
                const usize bits = message_bits(channel, size);
                if (bits > remaining) {
                    continue;  // Mảnh nhỏ hơn phía sau có thể còn vừa.
                }
                writer.write_bool(true);
                writer.write_bits(std::to_underlying(channel), kChannelBits);
                writer.write_bits(id, kSequenceBits);
                writer.write_ranged(slot.fragment_count, 1, kMaxFragments);
                writer.write_ranged(f, 0, kMaxFragments - 1);
                writer.write_bits(slot.offset, kOffsetBits);
                writer.write_bytes(std::span(out.ring).subspan(slot.offset + start, size),
                                   kMaxDataField);
                slot.sent |= bit;
                slot.last_sent[f] = now;
                record.fragments[record.count++] =
                    FragmentRef{.channel = std::to_underlying(channel),
                                .fragment = static_cast<u8>(f),
                                .id = id};
                remaining -= bits;
                ++written;
            }
        }
    }
}

void MessageChannels::State::drop_queued(const usize count) noexcept {
    if (count == 0) {
        return;
    }
    const usize bytes = count == queue_count ? queue_used : queue[count].offset;
    std::copy(queue_bytes.begin() + static_cast<std::ptrdiff_t>(bytes),
              queue_bytes.begin() + static_cast<std::ptrdiff_t>(queue_used), queue_bytes.begin());
    for (usize i = count; i < queue_count; ++i) {
        queue[i - count] = queue[i];
        queue[i - count].offset -= bytes;
    }
    queue_count -= count;
    queue_used -= bytes;
}

Result<void> MessageChannels::State::parse_message(BitReader& reader, ParsedMessage& message,
                                                   usize& scratch_used) noexcept {
    const Result<u64> channel = reader.read_bits(kChannelBits);
    if (!channel) {
        return std::unexpected(channel.error());
    }
    message = ParsedMessage{};
    message.channel = static_cast<Channel>(*channel);
    if (message.channel != Channel::Unreliable) {
        const Result<u64> number = reader.read_bits(kSequenceBits);
        if (!number) {
            return std::unexpected(number.error());
        }
        message.number = static_cast<u16>(*number);
    }
    if (is_reliable(message.channel)) {
        const Result<i64> count = reader.read_ranged(1, kMaxFragments);
        if (!count) {
            return std::unexpected(count.error());
        }
        const Result<i64> index = reader.read_ranged(0, kMaxFragments - 1);
        if (!index) {
            return std::unexpected(index.error());
        }
        const Result<u64> offset = reader.read_bits(kOffsetBits);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        message.fragment_count = static_cast<u32>(*count);
        message.fragment_index = static_cast<u32>(*index);
        message.offset = static_cast<usize>(*offset);
    }
    const Result<usize> size =
        reader.read_bytes(std::span(parse_scratch).subspan(scratch_used), kMaxDataField);
    if (!size) {
        return std::unexpected(size.error());
    }
    message.data = std::span(parse_scratch).subspan(scratch_used, *size);
    scratch_used += *size;
    if (*size == 0 || (!is_reliable(message.channel) && *size > kMaxUnreliableMessageSize)) {
        return fail(ErrorCode::InvalidArgument, "net: tin nhắn rỗng hay quá cỡ của kênh");
    }
    if (is_reliable(message.channel)) {
        return check_reliable(message);
    }
    return {};
}

Result<void> MessageChannels::State::check_reliable(ParsedMessage& message) const noexcept {
    const bool last = message.fragment_index + 1 == message.fragment_count;
    // Tin nhắn dài ít nhất `before` + 1 byte; đúng `before` + cỡ mảnh cuối khi đây là mảnh cuối.
    const usize before = usize{message.fragment_count - 1} * kFragmentSize;
    const usize known = before + (last ? message.data.size() : 1);
    const ReceiveChannel& in = receive[reliable_index(message.channel)];
    if (message.fragment_index >= message.fragment_count || message.data.size() > kFragmentSize ||
        (!last && message.data.size() != kFragmentSize) ||
        message.offset + known > in.buffer.size()) {
        return fail(ErrorCode::InvalidArgument, "net: mảnh tin cậy sai luật");
    }
    const u16 offset = diff16(message.number, in.first_undelivered);
    if (offset >= kHalfRange) {
        message.duplicate = true;  // Đã giao từ trước.
        return {};
    }
    if (offset >= kReliableWindow) {
        return fail(ErrorCode::InvalidArgument, "net: id tin nhắn ngoài cửa sổ nhận");
    }
    const ReceiveSlot& slot = in.slots[message.number % kReliableWindow];
    if (slot.state != SlotState::Free &&
        (slot.id != message.number || slot.fragment_count != message.fragment_count ||
         slot.offset != message.offset)) {
        return fail(ErrorCode::InvalidArgument, "net: số mảnh hay offset khác lần trước");
    }
    // Ô nhận chỉ phản ánh các gói trước; mảnh cùng tin nhắn trong chính gói này cũng phải khớp.
    for (u32 i = 0; i < parsed.count; ++i) {
        const ParsedMessage& earlier = parsed.messages[i];
        if (earlier.channel == message.channel && earlier.number == message.number &&
            (earlier.fragment_count != message.fragment_count ||
             earlier.offset != message.offset)) {
            return fail(ErrorCode::InvalidArgument,
                        "net: số mảnh hay offset khác mảnh trước trong gói");
        }
    }
    return {};
}

Result<void> MessageChannels::State::parse(const std::span<const std::byte> packet) noexcept {
    BitReader reader(packet);
    parsed = ParsedPacket{};
    const Result<u64> sequence = reader.read_bits(kSequenceBits);
    const Result<bool> has_ack = sequence ? reader.read_bool() : Result<bool>(false);
    if (!sequence || !has_ack) {
        return fail(ErrorCode::OutOfRange, "net: gói ngắn hơn header");
    }
    parsed.sequence = static_cast<u16>(*sequence);
    parsed.has_ack = *has_ack;
    if (parsed.has_ack) {
        const Result<u64> ack = reader.read_bits(kSequenceBits);
        const Result<u64> bits = ack ? reader.read_bits(kAckBitsCount) : Result<u64>(0U);
        if (!ack || !bits) {
            return fail(ErrorCode::OutOfRange, "net: gói ngắn hơn header");
        }
        parsed.ack = static_cast<u16>(*ack);
        parsed.ack_bits = static_cast<u32>(*bits);
    }
    usize scratch_used = 0;
    while (true) {
        const Result<bool> more = reader.read_bool();
        if (!more) {
            return std::unexpected(more.error());
        }
        if (!*more) {
            break;
        }
        if (parsed.count == kMaxMessagesPerPacket) {
            return fail(ErrorCode::InvalidArgument, "net: quá 64 tin nhắn trong một gói");
        }
        ParsedMessage& message = parsed.messages[parsed.count];
        if (const Result<void> read = parse_message(reader, message, scratch_used); !read) {
            return std::unexpected(read.error());
        }
        parsed.has_reliable = parsed.has_reliable || is_reliable(message.channel);
        ++parsed.count;
    }
    return reader.finish();
}

void MessageChannels::State::release_delivered() noexcept {
    for (ReceiveChannel& in : receive) {
        for (ReceiveSlot& slot : in.slots) {
            if (slot.state == SlotState::Delivered && newer16(in.first_undelivered, slot.id)) {
                slot = ReceiveSlot{};
            }
        }
    }
}

void MessageChannels::State::record_received(const u16 sequence) noexcept {
    if (!has_received) {
        has_received = true;
        received_highest = sequence;
        received_bits = 0;
        return;
    }
    if (newer16(sequence, received_highest)) {
        const u16 shift = diff16(sequence, received_highest);
        const u64 wide =
            shift > kAckBitsCount ? 0 : ((u64{received_bits} << shift) | (u64{1} << (shift - 1)));
        received_bits = static_cast<u32>(wide & 0xFFFF'FFFFU);
        received_highest = sequence;
        return;
    }
    const u16 age = diff16(received_highest, sequence);
    if (age >= 1 && age <= kAckBitsCount) {
        received_bits |= u32{1} << (age - 1);
    }
}

void MessageChannels::State::acknowledge(const u16 sequence, const core::MonoTime now) noexcept {
    SentPacket& packet = sent[sequence % kSentPacketHistory];
    if (!packet.valid || packet.acked || packet.sequence != sequence) {
        return;
    }
    packet.acked = true;
    // Mẫu âm chỉ có khi bên gọi đưa thời gian lùi; coi như 0 để RTT không âm.
    const core::Duration sample = std::max(core::Duration{}, now - packet.sent_at);
    rtt = rtt + (sample - rtt) / 8;
    for (u32 i = 0; i < packet.count; ++i) {
        const FragmentRef& ref = packet.fragments[i];
        SendChannel& out = send[reliable_index(static_cast<Channel>(ref.channel))];
        SendSlot& slot = out.slots[ref.id % kReliableWindow];
        if (!slot.used || slot.id != ref.id) {
            continue;
        }
        slot.acked |= u32{1} << ref.fragment;
        if (slot.acked == all_fragments(slot.fragment_count)) {
            slot.used = false;
        }
    }
    for (SendChannel& out : send) {
        while (out.oldest != out.next_id && !out.slots[out.oldest % kReliableWindow].used) {
            out.oldest = add16(out.oldest, 1);
        }
    }
}

void MessageChannels::State::apply_acks(const core::MonoTime now) noexcept {
    if (!parsed.has_ack) {
        return;
    }
    acknowledge(parsed.ack, now);
    for (u32 i = 0; i < kAckBitsCount; ++i) {
        if ((parsed.ack_bits & (u32{1} << i)) != 0) {
            acknowledge(static_cast<u16>(parsed.ack - 1 - i), now);
        }
    }
}

void MessageChannels::State::deliver(const Channel channel,
                                     const std::span<const std::byte> data) noexcept {
    ORION_VERIFY(delivered_count < delivered.size(), "net: quá nhiều tin nhắn giao trong một gói");
    delivered[delivered_count++] = ReceivedMessage{.channel = channel, .data = data};
}

void MessageChannels::State::store_fragment(const ParsedMessage& message) noexcept {
    ReceiveChannel& in = receive[reliable_index(message.channel)];
    ReceiveSlot& slot = in.slots[message.number % kReliableWindow];
    if (slot.state == SlotState::Free) {
        slot = ReceiveSlot{.state = SlotState::Receiving,
                           .id = message.number,
                           .fragment_count = message.fragment_count,
                           .received = 0,
                           .offset = message.offset,
                           .size = 0};
    }
    const u32 bit = u32{1} << message.fragment_index;
    if (slot.state != SlotState::Receiving || (slot.received & bit) != 0) {
        return;  // Mảnh đã có, hay tin nhắn đã đủ.
    }
    const usize at = slot.offset + (usize{message.fragment_index} * kFragmentSize);
    std::ranges::copy(message.data, in.buffer.begin() + static_cast<std::ptrdiff_t>(at));
    slot.received |= bit;
    if (message.fragment_index + 1 == message.fragment_count) {
        slot.size = (usize{message.fragment_count - 1} * kFragmentSize) + message.data.size();
    }
    if (slot.received != all_fragments(slot.fragment_count)) {
        return;
    }
    slot.state = SlotState::Complete;
    if (message.channel == Channel::ReliableUnordered) {
        slot.state = SlotState::Delivered;
        deliver(message.channel, std::span(in.buffer).subspan(slot.offset, slot.size));
    }
}

void MessageChannels::State::apply_message(const ParsedMessage& message) noexcept {
    switch (message.channel) {
        case Channel::Unreliable:
            deliver(message.channel, message.data);
            return;
        case Channel::Sequenced:
            if (!has_sequenced || newer16(message.number, last_sequenced)) {
                has_sequenced = true;
                last_sequenced = message.number;
                deliver(message.channel, message.data);
            }
            return;
        case Channel::ReliableOrdered:
        case Channel::ReliableUnordered:
            if (!message.duplicate) {
                store_fragment(message);
            }
            return;
    }
}

void MessageChannels::State::release_ordered() noexcept {
    ReceiveChannel& ordered = receive[reliable_index(Channel::ReliableOrdered)];
    while (true) {
        ReceiveSlot& slot = ordered.slots[ordered.first_undelivered % kReliableWindow];
        if (slot.state != SlotState::Complete || slot.id != ordered.first_undelivered) {
            break;
        }
        slot.state = SlotState::Delivered;
        deliver(Channel::ReliableOrdered,
                std::span(ordered.buffer).subspan(slot.offset, slot.size));
        ordered.first_undelivered = add16(ordered.first_undelivered, 1);
    }
    ReceiveChannel& unordered = receive[reliable_index(Channel::ReliableUnordered)];
    while (true) {
        const ReceiveSlot& slot = unordered.slots[unordered.first_undelivered % kReliableWindow];
        if (slot.state != SlotState::Delivered || slot.id != unordered.first_undelivered) {
            break;
        }
        unordered.first_undelivered = add16(unordered.first_undelivered, 1);
    }
}

MessageChannels::MessageChannels(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
MessageChannels::MessageChannels(MessageChannels&&) noexcept = default;
MessageChannels& MessageChannels::operator=(MessageChannels&&) noexcept = default;
MessageChannels::~MessageChannels() = default;

Result<MessageChannels> MessageChannels::create(const ChannelsConfig& config) {
    const auto valid = [](const usize size) {
        return size >= 1 && size <= kMaxReliableBufferSize;
    };
    if (!valid(config.receive_buffer_size) || !valid(config.send_buffer_size)) {
        return fail(ErrorCode::InvalidArgument, "net: cỡ bộ đệm tin cậy ngoài giới hạn");
    }
    return MessageChannels(std::make_unique<State>(config));
}

Result<void> MessageChannels::send(const Channel channel,
                                   const std::span<const std::byte> message) noexcept {
    if (message.empty()) {
        return fail(ErrorCode::InvalidArgument, "net: tin nhắn rỗng");
    }
    if (!is_reliable(channel)) {
        if (message.size() > kMaxUnreliableMessageSize) {
            return fail(ErrorCode::InvalidArgument, "net: tin nhắn unreliable quá lớn",
                        static_cast<i64>(message.size()));
        }
        return state_->queue_unreliable(channel, message);
    }
    return state_->queue_reliable(channel, message);
}

usize MessageChannels::write_packet(const std::span<std::byte, kMaxTransportPayload> out,
                                    const core::MonoTime now) noexcept {
    State& state = *state_;
    BitWriter writer(out);
    writer.write_bits(state.next_packet_sequence, kSequenceBits);
    writer.write_bool(state.has_received);
    if (state.has_received) {
        writer.write_bits(state.received_highest, kSequenceBits);
        writer.write_bits(state.received_bits, kAckBitsCount);
    }
    // Chừa một bit cho cờ kết thúc.
    usize remaining = (out.size() * 8) - writer.bit_count() - 1;
    u32 written = 0;
    SentPacket record;
    const usize taken = state.write_unreliable(writer, remaining, written);
    state.write_reliable(writer, remaining, written, record, now);
    writer.write_bool(false);
    ORION_VERIFY(!writer.overflowed(), "net: tính sai chỗ còn lại trong gói");
    const bool ack_due =
        state.ack_pending && (!state.has_sent || now - state.last_packet_sent >= kAckDelay);
    if (written == 0 && !ack_due) {
        return 0;
    }
    state.drop_queued(taken);
    record.valid = true;
    record.sequence = state.next_packet_sequence;
    record.sent_at = now;
    state.sent[state.next_packet_sequence % kSentPacketHistory] = record;
    state.next_packet_sequence = add16(state.next_packet_sequence, 1);
    state.last_packet_sent = now;
    state.has_sent = true;
    state.ack_pending = false;
    return writer.byte_count();
}

Result<std::span<const ReceivedMessage>> MessageChannels::read_packet(
    const std::span<const std::byte> packet, const core::MonoTime now) noexcept {
    State& state = *state_;
    state.release_delivered();
    state.delivered_count = 0;
    if (const Result<void> parsed = state.parse(packet); !parsed) {
        return std::unexpected(parsed.error());
    }
    // Từ đây gói đã được kiểm hết: áp.
    state.record_received(state.parsed.sequence);
    state.ack_pending = state.ack_pending || state.parsed.has_reliable;
    state.apply_acks(now);
    for (u32 i = 0; i < state.parsed.count; ++i) {
        state.apply_message(state.parsed.messages[i]);
    }
    state.release_ordered();
    return std::span<const ReceivedMessage>(state.delivered).first(state.delivered_count);
}

core::Duration MessageChannels::rtt() const noexcept {
    return state_->rtt;
}

u32 MessageChannels::in_flight(const Channel channel) const noexcept {
    if (!is_reliable(channel)) {
        return 0;
    }
    const SendChannel& out = state_->send[reliable_index(channel)];
    return diff16(out.next_id, out.oldest);
}

}  // namespace orion::net
