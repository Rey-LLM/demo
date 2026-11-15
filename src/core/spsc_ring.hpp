#pragma once
// Bounded wait-free single-producer / single-consumer ring (Lamport). Carries
// VisualEvents (effects that exist only as events, such as a live whale arc)
// from the model thread to the render thread. Full ring: the push fails and
// the producer counts a drop; neither side ever blocks.
//
// Ordering: the producer writes the slot, then release-stores `head_`; the
// consumer acquire-loads `head_` before reading the slot, and release-stores
// `tail_` after reading it, which the producer acquire-loads before reusing
// the slot. Each index is written by exactly one thread.

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

#include "core/aligned_allocator.hpp"

namespace ftv {

template <class T, std::size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0 && N >= 2, "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  bool try_push(const T& v) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= N) return false;
    slots_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(T& out) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = slots_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  std::array<T, N> slots_{};
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};

}  // namespace ftv
