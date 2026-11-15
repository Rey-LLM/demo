#pragma once
// Test-and-test-and-set spinlock used by the entity-registry shards.
//
// Why a spinlock instead of a mutex or a lock-free insert:
//  * Workers merge their thread-local batches in bulk, one lock acquisition per
//    (worker, shard) pair, so hold times are long-ish but acquisitions are
//    rare and contention across 64 shards is low. A futex-backed mutex would
//    add syscalls for no benefit; a lock-free open-addressing insert cannot
//    grow the table without a far more complex migration protocol.
//  * memory_order_acquire on lock / release on unlock is exactly the
//    critical-section contract: everything written while holding the lock
//    happens-before the next owner's reads. The relaxed load in the spin loop
//    only polls; the subsequent exchange re-establishes ordering.

#include <atomic>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define FTV_CPU_RELAX() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#define FTV_CPU_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define FTV_CPU_RELAX() asm volatile("yield" ::: "memory")
#else
#define FTV_CPU_RELAX() ((void)0)
#endif

namespace ftv {

class SpinLock {
 public:
  void lock() noexcept {
    for (unsigned spins = 0;; ++spins) {
      if (!flag_.exchange(true, std::memory_order_acquire)) return;
      while (flag_.load(std::memory_order_relaxed)) {
        if (++spins < 64) {
          FTV_CPU_RELAX();
        } else {
          std::this_thread::yield();
        }
      }
    }
  }
  bool try_lock() noexcept {
    return !flag_.load(std::memory_order_relaxed) && !flag_.exchange(true, std::memory_order_acquire);
  }
  void unlock() noexcept { flag_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> flag_{false};
};

}  // namespace ftv
