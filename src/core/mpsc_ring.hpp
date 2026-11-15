#pragma once
// Bounded lock-free multi-producer / single-consumer ring for live events.
//
// Producers are the feed threads (one per adapter, plus the per-feed replay
// fallbacks); the consumer is the model thread. try_push() never blocks and
// never allocates: when the ring is full it returns false and the producer
// counts a drop. A network thread must never wait on the model.
//
// Algorithm: Vyukov's per-cell sequence numbers (see mpmc_queue.hpp). With a
// single consumer the dequeue side needs no CAS: the consumer owns
// `dequeue_pos_` outright.
//  * Producers claim a cell with a relaxed CAS on `enqueue_pos_` (it only
//    arbitrates ownership) and publish it with a release store of seq.
//  * The consumer's acquire load of seq makes the payload visible; its
//    release store of seq + capacity hands the cell back to producers, whose
//    acquire load then knows the consumer has finished reading it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/aligned_allocator.hpp"

namespace ftv {

template <class T>
class MpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "MpscRing payloads must be trivially copyable");

 public:
  explicit MpscRing(std::size_t capacityPow2) : mask_(round_up(capacityPow2) - 1), cells_(mask_ + 1) {
    for (std::size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  MpscRing(const MpscRing&) = delete;
  MpscRing& operator=(const MpscRing&) = delete;

  // Any thread. Returns false (and the caller counts a drop) when full.
  bool try_push(const T& value) noexcept {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[pos & mask_];
      const std::size_t seq = cell.seq.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          cell.data = value;
          cell.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // full: the consumer has not freed this cell yet
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  // Consumer thread only.
  bool try_pop(T& out) noexcept {
    Cell& cell = cells_[dequeue_pos_ & mask_];
    const std::size_t seq = cell.seq.load(std::memory_order_acquire);
    if (seq != dequeue_pos_ + 1) return false;  // empty (or a producer is mid-write)
    out = cell.data;
    cell.seq.store(dequeue_pos_ + mask_ + 1, std::memory_order_release);
    ++dequeue_pos_;
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }
  // Approximate fill level (display only).
  [[nodiscard]] std::size_t approx_size() const noexcept {
    const std::size_t e = enqueue_pos_.load(std::memory_order_relaxed);
    const std::size_t d = dequeue_seen_.load(std::memory_order_relaxed);
    return e >= d ? e - d : 0;
  }
  // Consumer: publish its position for approx_size() (relaxed, display only).
  void publish_consumer_position() noexcept { dequeue_seen_.store(dequeue_pos_, std::memory_order_relaxed); }

 private:
  static std::size_t round_up(std::size_t v) noexcept {
    std::size_t p = 2;
    while (p < v) p <<= 1;
    return p;
  }
  struct alignas(kCacheLine) Cell {
    std::atomic<std::size_t> seq{0};
    T data{};
  };

  const std::size_t mask_;
  AlignedVector<Cell> cells_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::size_t dequeue_pos_ = 0;
  std::atomic<std::size_t> dequeue_seen_{0};
};

}  // namespace ftv
