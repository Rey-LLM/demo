#include "core/thread_pool.hpp"

#include <algorithm>

namespace ftv {

ThreadPool::ThreadPool(unsigned workerCount) {
  workers_.reserve(workerCount);
  for (unsigned i = 0; i < workerCount; ++i) {
    workers_.emplace_back([this, slot = i + 1](std::stop_token st) { worker_main(st, slot); });
  }
}

ThreadPool::~ThreadPool() {
  // seq_cst is fine here; shutdown is not performance sensitive.
  stopping_.store(true);
  generation_.fetch_add(1);
  generation_.notify_all();
  for (auto& w : workers_) w.request_stop();
  workers_.clear();  // joins
}

void ThreadPool::run(std::size_t count, std::size_t grain, RangeFn fn, void* ctx) {
  std::lock_guard<std::mutex> guard(submit_mutex_);
  job_fn_ = fn;
  job_ctx_ = ctx;
  job_count_ = count;
  job_grain_ = grain;
  job_chunks_ = chunk_count(count, grain);
  // relaxed: these stores are published by the release RMW on generation_.
  next_chunk_.store(0, std::memory_order_relaxed);
  pending_workers_.store(static_cast<unsigned>(workers_.size()), std::memory_order_relaxed);
  // release: makes the job fields above visible to any worker whose acquire
  // load observes the new generation value.
  generation_.fetch_add(1, std::memory_order_release);
  generation_.notify_all();

  drain(0);

  // acquire: pairs with the workers' acq_rel decrement so every write a worker
  // made inside the job happens-before parallel_for returns.
  unsigned pending = pending_workers_.load(std::memory_order_acquire);
  while (pending != 0) {
    pending_workers_.wait(pending, std::memory_order_acquire);
    pending = pending_workers_.load(std::memory_order_acquire);
  }
}

void ThreadPool::drain(unsigned slot) noexcept {
  const std::size_t chunks = job_chunks_;
  const std::size_t grain = job_grain_;
  const std::size_t count = job_count_;
  for (;;) {
    // relaxed: the counter only distributes indices; job data visibility was
    // established by the acquire on generation_.
    const std::size_t k = next_chunk_.fetch_add(1, std::memory_order_relaxed);
    if (k >= chunks) break;
    const std::size_t b = k * grain;
    job_fn_(job_ctx_, b, std::min(count, b + grain), slot);
  }
}

void ThreadPool::worker_main(std::stop_token st, unsigned slot) {
  // Start from generation 0, not from the current value: a worker that starts
  // late must still take part in any job submitted before it began running
  // (run() waits for every worker), and must observe a shutdown that already
  // happened.
  std::uint64_t seen = 0;
  for (;;) {
    generation_.wait(seen, std::memory_order_acquire);
    // acquire: see the job description stored before the release increment.
    const std::uint64_t g = generation_.load(std::memory_order_acquire);
    if (stopping_.load(std::memory_order_acquire) || st.stop_requested()) return;
    if (g == seen) continue;
    seen = g;
    drain(slot);
    // acq_rel: release publishes this worker's job writes to the submitter;
    // acquire is harmless and keeps the RMW chain simple to reason about.
    if (pending_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      pending_workers_.notify_all();
    }
  }
}

unsigned default_parser_threads() noexcept {
  const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
  return hc > 3 ? hc - 2 : 1u;
}

unsigned default_pool_workers() noexcept {
  const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
  return hc > 1 ? hc - 1 : 0u;
}

}  // namespace ftv
