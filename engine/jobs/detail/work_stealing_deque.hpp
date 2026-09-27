#pragma once

// Deque Chase–Lev có giới hạn cho work stealing (Chase, Lev 2005), theo bản đã chứng minh cho mô
// hình bộ nhớ C11 của Lê, Pop, Cohen, Zappa Nardelli ("Correct and Efficient Work-Stealing for
// Weak Memory Models", PPoPP 2013). Một luồng chủ đẩy và lấy ở đáy (LIFO, giữ cache nóng); mọi
// luồng khác trộm ở đỉnh (FIFO, lấy việc lớn nhất còn lại).
//
// Đồng bộ:
// - bottom_ chỉ luồng chủ ghi; top_ đổi bằng CAS của bất kỳ ai lấy phần tử cuối hoặc trộm.
// - Luồng chủ ghi ô rồi store bottom_ bằng release; luồng trộm load bottom_ bằng acquire trước khi
//   đọc ô, nên thấy dữ liệu của ô và mọi thứ luồng chủ ghi trước khi đẩy.
// - Hàng rào seq_cst trong pop và steal xếp thứ tự "ghi bottom_ rồi đọc top_" với "đọc top_ rồi
//   đọc bottom_", để luồng chủ và luồng trộm không cùng lấy phần tử cuối (bài toán Dekker).
// - Mỗi ô là các atomic<u64> đọc ghi relaxed: luồng trộm có thể đọc ô mà luồng chủ đang ghi lại
//   sau một vòng; giá trị đó bị bỏ vì CAS lên top_ của luồng trộm khi ấy chắc chắn thua.

#include "engine/core/queue.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <vector>

namespace orion::jobs::detail {

template <class T>
class WorkStealingDeque {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) % sizeof(u64) == 0 && alignof(T) <= alignof(u64));
    static constexpr usize kWords = sizeof(T) / sizeof(u64);
    using Words = std::array<u64, kWords>;
    using Slot = std::array<std::atomic<u64>, kWords>;

public:
    // Sức chứa làm tròn lên lũy thừa của 2, tối thiểu 2.
    explicit WorkStealingDeque(const usize capacity)
        : slots_(std::bit_ceil(capacity < 2 ? usize{2} : capacity)),
          mask_(static_cast<i64>(slots_.size()) - 1) {}

    // Chỉ luồng chủ. false khi đầy.
    [[nodiscard]] bool push(const T& value) noexcept {
        // relaxed: chỉ luồng chủ ghi bottom_.
        const i64 bottom = bottom_.load(std::memory_order_relaxed);
        // acquire: thấy các lần trộm đã xong trước khi dùng lại ô của chúng.
        const i64 top = top_.load(std::memory_order_acquire);
        if (bottom - top > mask_) {
            return false;
        }
        store(bottom, value);
        // release: công bố ô vừa ghi cho luồng trộm load bottom_ bằng acquire.
        bottom_.store(bottom + 1, std::memory_order_release);
        return true;
    }

    // Chỉ luồng chủ; lấy phần tử mới nhất.
    [[nodiscard]] std::optional<T> pop() noexcept {
        // relaxed: chỉ luồng chủ ghi bottom_; thứ tự với top_ do hàng rào bên dưới lo.
        const i64 bottom = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(bottom, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // relaxed: hàng rào seq_cst ở trên đã xếp thứ tự lần đọc này.
        i64 top = top_.load(std::memory_order_relaxed);
        if (top > bottom) {
            // Rỗng: trả bottom_ về như cũ. relaxed: chỉ luồng chủ ghi.
            bottom_.store(bottom + 1, std::memory_order_relaxed);
            return std::nullopt;
        }
        const T value = load(bottom);
        if (top == bottom) {
            // Phần tử cuối: đua với luồng trộm bằng CAS trên top_.
            const bool won = top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                                          std::memory_order_relaxed);
            // relaxed: chỉ luồng chủ ghi bottom_.
            bottom_.store(bottom + 1, std::memory_order_relaxed);
            if (!won) {
                return std::nullopt;
            }
        }
        return value;
    }

    // Mọi luồng khác luồng chủ; lấy phần tử cũ nhất. nullopt khi rỗng, hoặc khi thua cuộc đua với
    // một luồng khác (deque có thể vẫn còn phần tử).
    [[nodiscard]] std::optional<T> steal() noexcept {
        // acquire: ghép với CAS của lần trộm trước.
        i64 top = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // acquire: ghép với release của push để thấy nội dung ô.
        const i64 bottom = bottom_.load(std::memory_order_acquire);
        if (top >= bottom) {
            return std::nullopt;
        }
        const T value = load(top);
        if (!top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] usize capacity() const noexcept { return slots_.size(); }

private:
    void store(const i64 index, const T& value) noexcept {
        const auto words = std::bit_cast<Words>(value);
        Slot& slot = slots_[static_cast<usize>(index & mask_)];
        for (usize i = 0; i < kWords; ++i) {
            // relaxed: được công bố bằng release trên bottom_.
            slot[i].store(words[i], std::memory_order_relaxed);
        }
    }

    [[nodiscard]] T load(const i64 index) const noexcept {
        Words words{};
        const Slot& slot = slots_[static_cast<usize>(index & mask_)];
        for (usize i = 0; i < kWords; ++i) {
            // relaxed: đã thứ tự bằng acquire trên bottom_ (trộm) hoặc do chính luồng chủ ghi.
            words[i] = slot[i].load(std::memory_order_relaxed);
        }
        return std::bit_cast<T>(words);
    }

    std::vector<Slot> slots_;
    i64 mask_;
    [[maybe_unused]] std::array<std::byte, core::kFalseSharingDistance> pad0_{};
    std::atomic<i64> top_{0};
    [[maybe_unused]] std::array<std::byte, core::kFalseSharingDistance> pad1_{};
    std::atomic<i64> bottom_{0};
    [[maybe_unused]] std::array<std::byte, core::kFalseSharingDistance> pad2_{};
};

}  // namespace orion::jobs::detail
