#include "engine/net/replay_window.hpp"

#include "engine/core/types.hpp"

#include <optional>

namespace orion::net {

bool ReplayWindow::fresh(const u64 sequence) const noexcept {
    if (empty_ || sequence > highest_) {
        return true;
    }
    return highest_ - sequence < kSize && !seen(sequence);
}

void ReplayWindow::record(const u64 sequence) noexcept {
    if (!fresh(sequence)) {
        return;
    }
    if (empty_ || sequence > highest_) {
        // Các ô của số từ highest_ + 1 tới sequence đang giữ bit của số đã rơi khỏi cửa sổ: xoá.
        if (empty_ || sequence - highest_ >= kSize) {
            bits_.fill(0);
        } else {
            for (u64 s = highest_ + 1; s != sequence; ++s) {
                set(s, false);
            }
        }
        highest_ = sequence;
        empty_ = false;
    }
    set(sequence, true);
}

std::optional<u64> ReplayWindow::highest() const noexcept {
    if (empty_) {
        return std::nullopt;
    }
    return highest_;
}

bool ReplayWindow::seen(const u64 sequence) const noexcept {
    const u64 slot = sequence % kSize;
    return ((bits_[slot / kWordBits] >> (slot % kWordBits)) & 1U) != 0;
}

void ReplayWindow::set(const u64 sequence, const bool value) noexcept {
    const u64 slot = sequence % kSize;
    const u64 mask = u64{1} << (slot % kWordBits);
    u64& word = bits_[slot / kWordBits];
    word = value ? (word | mask) : (word & ~mask);
}

}  // namespace orion::net
