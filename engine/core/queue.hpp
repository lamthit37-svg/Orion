#pragma once

// Hàng đợi vòng không khoá, sức chứa cố định, để các luồng trao dữ liệu cho nhau (CLAUDE.md X.7:
// hot path của mô phỏng không dùng khoá; luồng IO mạng và luồng mô phỏng trao gói qua hàng đợi).
//
// - SpscRing: đúng một luồng đẩy, đúng một luồng lấy. Rẻ nhất.
// - MpmcRing: nhiều luồng đẩy, nhiều luồng lấy (hàng đợi có giới hạn của Dmitry Vyukov). Dùng làm
//   MPSC cho logger.
//
// Cả hai cấp toàn bộ bộ nhớ lúc dựng; try_push và try_pop không bao giờ cấp phát, không bao giờ
// chặn: đầy thì try_push trả false, rỗng thì try_pop trả false.

#include "engine/core/assert.hpp"
#include "engine/core/types.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <utility>
#include <vector>

namespace orion::core {

// Khoảng cách tối thiểu giữa hai biến nóng của hai luồng khác nhau để không chung dòng cache.
// Apple arm64 có dòng cache 128 byte; 128 cũng an toàn cho x64 (prefetcher theo cặp dòng 64 byte).
inline constexpr usize kFalseSharingDistance = 128;

// Một luồng đẩy, một luồng lấy. Mỗi phía giữ bản sao vị trí của phía kia để đọc atomic của phía kia
// chỉ khi bản sao cho thấy đầy hoặc rỗng.
//
// Đồng bộ: `tail_` do luồng đẩy ghi (release) sau khi ghi ô, luồng lấy đọc (acquire) trước khi đọc
// ô; `head_` ngược lại. Không có dữ liệu nào khác dùng chung.
template <class T>
class SpscRing {
public:
    // Sức chứa làm tròn lên lũy thừa của 2, tối thiểu 2.
    explicit SpscRing(const usize capacity)
        : slots_(std::bit_ceil(capacity < 2 ? usize{2} : capacity)), mask_(slots_.size() - 1) {}

    // Chỉ luồng đẩy gọi.
    [[nodiscard]] bool try_push(T value) noexcept {
        // relaxed: chỉ luồng đẩy ghi tail_, nên luồng đẩy đọc lại giá trị của chính nó.
        const usize tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == slots_.size()) {
            // acquire: thấy head_ mới thì ô tương ứng đã được luồng lấy đọc xong.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == slots_.size()) {
                return false;
            }
        }
        slots_[tail & mask_] = std::move(value);
        // release: luồng lấy thấy tail mới thì thấy luôn giá trị vừa ghi vào ô.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Chỉ luồng lấy gọi.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        // relaxed: chỉ luồng lấy ghi head_.
        const usize head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            // acquire: ghép với release của try_push để thấy giá trị trong ô.
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) {
                return false;
            }
        }
        out = std::move(slots_[head & mask_]);
        // release: luồng đẩy thấy head mới thì được phép ghi đè ô này.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] usize capacity() const noexcept { return slots_.size(); }

private:
    std::vector<T> slots_;
    usize mask_;
    [[maybe_unused]] std::array<std::byte, kFalseSharingDistance> pad0_{};
    std::atomic<usize> head_{0};  // luồng lấy ghi
    usize tail_cache_ = 0;        // của luồng lấy
    [[maybe_unused]] std::array<std::byte, kFalseSharingDistance> pad1_{};
    std::atomic<usize> tail_{0};  // luồng đẩy ghi
    usize head_cache_ = 0;        // của luồng đẩy
    [[maybe_unused]] std::array<std::byte, kFalseSharingDistance> pad2_{};
};

// Nhiều luồng đẩy, nhiều luồng lấy. Mỗi ô có số thứ tự: ô sẵn để ghi ở lượt `pos` khi số thứ tự
// bằng `pos`, sẵn để đọc khi bằng `pos + 1`.
//
// Đồng bộ: vị trí đẩy và lấy được giành bằng CAS relaxed (chỉ để phân chia ô, không mang dữ liệu);
// dữ liệu của ô được công bố bằng store release lên số thứ tự và đọc sau load acquire của nó.
template <class T>
class MpmcRing {
public:
    // Sức chứa làm tròn lên lũy thừa của 2, tối thiểu 2.
    explicit MpmcRing(const usize capacity)
        : cells_(std::bit_ceil(capacity < 2 ? usize{2} : capacity)), mask_(cells_.size() - 1) {
        for (usize i = 0; i < cells_.size(); ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool try_push(T value) noexcept {
        usize pos = enqueue_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        while (true) {
            cell = &cells_[pos & mask_];
            // acquire: ô đã được luồng lấy trước đó đọc xong trước khi ta ghi đè.
            const usize sequence = cell->sequence.load(std::memory_order_acquire);
            const auto lag = static_cast<isize>(sequence) - static_cast<isize>(pos);
            if (lag == 0) {
                // relaxed: CAS chỉ giành quyền ghi ô, dữ liệu được công bố bằng sequence bên dưới.
                if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (lag < 0) {
                return false;  // đầy
            } else {
                pos = enqueue_.load(std::memory_order_relaxed);
            }
        }
        cell->value = std::move(value);
        // release: công bố giá trị cho luồng lấy.
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        usize pos = dequeue_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        while (true) {
            cell = &cells_[pos & mask_];
            // acquire: ghép với release của try_push để thấy giá trị trong ô.
            const usize sequence = cell->sequence.load(std::memory_order_acquire);
            const auto lag = static_cast<isize>(sequence) - static_cast<isize>(pos + 1);
            if (lag == 0) {
                // relaxed: như try_push.
                if (dequeue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (lag < 0) {
                return false;  // rỗng
            } else {
                pos = dequeue_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->value);
        // release: trả ô cho vòng ghi kế tiếp.
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] usize capacity() const noexcept { return cells_.size(); }

private:
    struct Cell {
        std::atomic<usize> sequence{0};
        T value{};
    };

    std::vector<Cell> cells_;
    usize mask_;
    [[maybe_unused]] std::array<std::byte, kFalseSharingDistance> pad0_{};
    std::atomic<usize> enqueue_{0};
    [[maybe_unused]] std::array<std::byte, kFalseSharingDistance> pad1_{};
    std::atomic<usize> dequeue_{0};
    [[maybe_unused]] std::array<std::byte, kFalseSharingDistance> pad2_{};
};

}  // namespace orion::core
