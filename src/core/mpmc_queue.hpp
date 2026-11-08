#pragma once
// Bounded lock-free multi-producer / multi-consumer queue (Dmitry Vyukov's
// sequence-number ring). Used to distribute input chunks to parser workers.
//
// Each cell carries a sequence number. A producer may write cell i when
// seq == pos, a consumer may read it when seq == pos + 1. Memory ordering:
//  * The seq load uses acquire so a consumer that sees seq == pos+1 also sees
//    the payload written before the producer's release store, and a producer
//    that sees seq == pos sees that the previous consumer finished reading.
//  * The position CAS is relaxed: it only arbitrates ownership of a cell; the
//    payload hand-off is ordered entirely by the per-cell seq acquire/release.

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <type_traits>

#include "core/aligned_allocator.hpp"
#include "core/spinlock.hpp"

namespace ftv {

template <class T>
class MPMCQueue {
  static_assert(std::is_trivially_copyable_v<T>, "MPMCQueue payloads must be trivially copyable");

 public:
  explicit MPMCQueue(std::size_t capacityPow2) : mask_(round_up_pow2(capacityPow2) - 1), cells_(mask_ + 1) {
    for (std::size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

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
        return false;  // full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  bool try_pop(T& out) noexcept {
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[pos & mask_];
      const std::size_t seq = cell.seq.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = cell.data;
          cell.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  // Blocking push with spin-then-yield back-off.
  void push(const T& value) noexcept {
    for (unsigned spins = 0; !try_push(value); ++spins) backoff(spins);
  }

  // Blocking pop. Returns false once the queue is closed and drained.
  bool pop(T& out) noexcept {
    for (unsigned spins = 0;; ++spins) {
      if (try_pop(out)) return true;
      // acquire: pairs with close()'s release so items pushed before close()
      // are visible to the final try_pop below.
      if (closed_.load(std::memory_order_acquire)) return try_pop(out);
      backoff(spins);
    }
  }

  void close() noexcept { closed_.store(true, std::memory_order_release); }

 private:
  struct alignas(kCacheLine) Cell {
    std::atomic<std::size_t> seq{0};
    T data{};
  };

  static std::size_t round_up_pow2(std::size_t v) noexcept {
    std::size_t p = 2;
    while (p < v) p <<= 1;
    return p;
  }
  static void backoff(unsigned spins) noexcept {
    if (spins < 64) {
      FTV_CPU_RELAX();
    } else {
      std::this_thread::yield();
    }
  }

  const std::size_t mask_;
  AlignedVector<Cell> cells_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
  alignas(kCacheLine) std::atomic<bool> closed_{false};
};

}  // namespace ftv
