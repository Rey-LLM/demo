#pragma once
// Lock-free single-producer / single-consumer triple buffer.
//
// Three slots: the writer owns `back`, the reader owns `front`, and `middle`
// is shared through one atomic byte (slot index plus a FRESH bit). Neither
// side ever blocks or takes a mutex, which is what the render path requires.
//
//  writer: fill back(); publish() swaps back <-> middle and sets FRESH.
//  reader: acquire_latest() swaps front <-> middle iff FRESH is set.
//
// Memory ordering: both swaps are acq_rel exchanges on the same atomic.
//  * release (writer) makes the slot contents written before publish() visible
//    to the reader whose exchange acquires that value;
//  * acquire (writer) guarantees that the slot handed back by the reader is no
//    longer being read before the writer starts overwriting it, and vice versa.
// The relaxed pre-check in acquire_latest() only skips the RMW when nothing is
// new; it carries no data dependency.

#include <array>
#include <atomic>
#include <cstdint>

#include "core/aligned_allocator.hpp"

namespace ftv {

template <class T>
class TripleBuffer {
 public:
  TripleBuffer() = default;
  TripleBuffer(const TripleBuffer&) = delete;
  TripleBuffer& operator=(const TripleBuffer&) = delete;

  // Initialisation access (before any thread starts using the buffer).
  T& slot(unsigned i) noexcept { return slots_[i]; }

  // ---- writer side --------------------------------------------------------
  T& back() noexcept { return slots_[back_]; }
  void publish() noexcept {
    const std::uint8_t prev = middle_.exchange(static_cast<std::uint8_t>(back_ | kFresh), std::memory_order_acq_rel);
    back_ = static_cast<std::uint8_t>(prev & kIndexMask);
  }

  // ---- reader side --------------------------------------------------------
  // Returns true if a newer snapshot became the front slot.
  bool acquire_latest() noexcept {
    if ((middle_.load(std::memory_order_relaxed) & kFresh) == 0) return false;
    const std::uint8_t prev = middle_.exchange(front_, std::memory_order_acq_rel);
    front_ = static_cast<std::uint8_t>(prev & kIndexMask);
    return true;
  }
  const T& front() const noexcept { return slots_[front_]; }

 private:
  static constexpr std::uint8_t kFresh = 0x4;
  static constexpr std::uint8_t kIndexMask = 0x3;

  std::array<T, 3> slots_{};
  alignas(kCacheLine) std::atomic<std::uint8_t> middle_{1};
  alignas(kCacheLine) std::uint8_t back_ = 0;   // writer-private
  alignas(kCacheLine) std::uint8_t front_ = 2;  // reader-private
};

}  // namespace ftv
