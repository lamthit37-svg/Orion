#include "engine/net/outbox.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"
#include "engine/net/address.hpp"
#include "engine/net/packet.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

namespace orion::net {

Outbox::Outbox(const usize capacity) : packets_(capacity) {}

bool Outbox::push(const Address& to, const std::span<const std::byte> packet) noexcept {
    ORION_ASSERT(packet.size() <= kMaxPacketSize, "gói {} byte vượt kMaxPacketSize", packet.size());
    if (size_ == packets_.size()) {
        ++dropped_;
        return false;
    }
    OutgoingPacket& slot = packets_[size_];
    slot.to = to;
    slot.size = std::min(packet.size(), kMaxPacketSize);
    std::ranges::copy(packet.first(slot.size), slot.bytes.begin());
    ++size_;
    return true;
}

}  // namespace orion::net
