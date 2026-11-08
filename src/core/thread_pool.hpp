#pragma once
// Fixed-size thread pool specialised for allocation-free data-parallel loops.
//
// parallel_for() partitions [0, count) into fixed chunks of `grain` items and
// hands chunks out through an atomic counter. The calling thread participates.
// Because the chunk partition depends only on (count, grain) and never on the
// number of threads, any per-chunk reduction combined in chunk order is
// bit-for-bit deterministic regardless of scheduling.
//
// No std::function, no task queue nodes: a job is a function pointer plus a
// context pointer to the caller's lambda on the caller's stack, so dispatching
// a job performs zero heap allocations (required by the layout hot loop).
//
// Calls from different threads are serialised by an internal mutex (only the
// ingest and layout coordinators use the pool; the render thread never does).
// parallel_for must not be called re-entrantly from inside a job.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/aligned_allocator.hpp"

namespace ftv {

class ThreadPool {
 public:
  // workerCount background threads are spawned; the caller of parallel_for is
  // an additional participant, so concurrency() == workerCount + 1.
  explicit ThreadPool(unsigned workerCount);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  [[nodiscard]] unsigned concurrency() const noexcept { return static_cast<unsigned>(workers_.size()) + 1u; }

  [[nodiscard]] static constexpr std::size_t chunk_count(std::size_t count, std::size_t grain) noexcept {
    return grain == 0 ? 0 : (count + grain - 1) / grain;
  }

  // fn(begin, end, slot): slot is a stable participant index in
  // [0, concurrency()) usable to address per-thread scratch memory.
  template <class F>
  void parallel_for(std::size_t count, std::size_t grain, F&& fn) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    using Fn = std::remove_reference_t<F>;
    if (workers_.empty() || count <= grain) {
      for (std::size_t b = 0; b < count; b += grain) {
        fn(b, b + grain < count ? b + grain : count, 0u);
      }
      return;
    }
    run(count, grain, &invoke<Fn>, static_cast<void*>(&fn));
  }

  // Convenience overload for callers that do not need the slot.
  template <class F>
  void parallel_for_range(std::size_t count, std::size_t grain, F&& fn) {
    parallel_for(count, grain, [&fn](std::size_t b, std::size_t e, unsigned) { fn(b, e); });
  }

 private:
  using RangeFn = void (*)(void*, std::size_t, std::size_t, unsigned);

  template <class Fn>
  static void invoke(void* ctx, std::size_t b, std::size_t e, unsigned slot) {
    (*static_cast<Fn*>(ctx))(b, e, slot);
  }

  void run(std::size_t count, std::size_t grain, RangeFn fn, void* ctx);
  void drain(unsigned slot) noexcept;
  void worker_main(std::stop_token st, unsigned slot);

  std::mutex submit_mutex_;  // serialises jobs; never touched by the render thread

  // Job description. Written by the submitting thread before the release
  // increment of generation_, read by workers after their acquire load.
  RangeFn job_fn_ = nullptr;
  void* job_ctx_ = nullptr;
  std::size_t job_count_ = 0;
  std::size_t job_grain_ = 0;
  std::size_t job_chunks_ = 0;

  alignas(kCacheLine) std::atomic<std::uint64_t> generation_{0};
  alignas(kCacheLine) std::atomic<std::size_t> next_chunk_{0};
  alignas(kCacheLine) std::atomic<unsigned> pending_workers_{0};
  alignas(kCacheLine) std::atomic<bool> stopping_{false};

  std::vector<std::jthread> workers_;
};

// N = max(1, hardware_concurrency - 2) as mandated for parser workers.
unsigned default_parser_threads() noexcept;
// Background workers for the shared pool: hardware_concurrency - 1 (caller joins).
unsigned default_pool_workers() noexcept;

}  // namespace ftv
