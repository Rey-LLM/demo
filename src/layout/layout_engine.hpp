#pragma once
// T_layout: the coordinator thread that drives ForceSolver iterations and
// hands position snapshots to the render thread through a lock-free triple
// buffer. Parallel force passes run on the shared ThreadPool.
//
// Control (pause / reheat / stop) uses atomics plus std::atomic::wait on a
// control counter; nothing here ever blocks the render thread.

#include <atomic>
#include <cstdint>
#include <thread>

#include "core/aligned_allocator.hpp"
#include "core/triple_buffer.hpp"
#include "layout/force_solver.hpp"

namespace ftv {

struct PositionSnapshot {
  AlignedVector<float> xyz;  // interleaved, 3 floats per node, preallocated
  std::uint64_t iteration = 0;
};

struct LayoutStatus {
  std::uint64_t iteration = 0;
  float iteration_ms = 0.0f;
  float tree_ms = 0.0f;
  float repulsion_ms = 0.0f;
  float integrate_ms = 0.0f;
  float mean_displacement = 0.0f;
  float temperature = 0.0f;
  bool converged = false;
  bool paused = false;
  bool running = false;  // false while the layout thread is not started (screenshot runs)
};

class LayoutEngine {
 public:
  LayoutEngine(ForceSolver& solver, std::size_t nodeCount);
  ~LayoutEngine();
  LayoutEngine(const LayoutEngine&) = delete;
  LayoutEngine& operator=(const LayoutEngine&) = delete;

  void start();
  void stop();

  void set_paused(bool paused) noexcept;
  [[nodiscard]] bool paused() const noexcept { return paused_.load(std::memory_order_relaxed); }
  void reheat() noexcept;

  // Render-thread side of the triple buffer.
  TripleBuffer<PositionSnapshot>& snapshots() noexcept { return snapshots_; }

  // Racy-but-safe snapshot of the status counters for the HUD: each field is
  // an independent relaxed atomic (values may come from adjacent iterations).
  [[nodiscard]] LayoutStatus status() const noexcept;

  // Publishes the solver's current positions (called before start()). When
  // `last` is given (iterations run before the engine existed), its timings
  // are reported through status() as well.
  void publish_now(const IterationStats* last = nullptr);

 private:
  void thread_main(std::stop_token st);
  void wake() noexcept;
  void publish_iteration(const IterationStats& s);

  ForceSolver& solver_;
  TripleBuffer<PositionSnapshot> snapshots_;
  std::jthread thread_;

  alignas(kCacheLine) std::atomic<std::uint32_t> control_{0};
  std::atomic<bool> paused_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> reheat_requested_{false};

  alignas(kCacheLine) std::atomic<std::uint64_t> st_iteration_{0};
  std::atomic<float> st_iteration_ms_{0.0f};
  std::atomic<float> st_tree_ms_{0.0f};
  std::atomic<float> st_repulsion_ms_{0.0f};
  std::atomic<float> st_integrate_ms_{0.0f};
  std::atomic<float> st_mean_disp_{0.0f};
  std::atomic<float> st_temperature_{0.0f};
  std::atomic<bool> st_converged_{false};
};

}  // namespace ftv
