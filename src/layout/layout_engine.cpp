#include "layout/layout_engine.hpp"

namespace ftv {

LayoutEngine::LayoutEngine(ForceSolver& solver, std::size_t nodeCount) : solver_(solver) {
  for (unsigned i = 0; i < 3; ++i) snapshots_.slot(i).xyz.assign(nodeCount * 3, 0.0f);
}

LayoutEngine::~LayoutEngine() { stop(); }

void LayoutEngine::start() {
  if (thread_.joinable()) return;
  running_.store(true, std::memory_order_relaxed);  // display-only flag
  thread_ = std::jthread([this](std::stop_token st) { thread_main(st); });
}

void LayoutEngine::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  wake();
  thread_.join();
  running_.store(false, std::memory_order_relaxed);
}

void LayoutEngine::wake() noexcept {
  // release: state changes made before wake() (paused_, reheat_requested_)
  // are visible to the layout thread after its acquire load of control_.
  control_.fetch_add(1, std::memory_order_release);
  control_.notify_all();
}

void LayoutEngine::set_paused(bool paused) noexcept {
  paused_.store(paused, std::memory_order_relaxed);  // ordered by wake()'s release
  wake();
}

void LayoutEngine::reheat() noexcept {
  reheat_requested_.store(true, std::memory_order_relaxed);  // ordered by wake()'s release
  wake();
}

LayoutStatus LayoutEngine::status() const noexcept {
  // relaxed everywhere: display-only counters with no dependent data.
  LayoutStatus s;
  s.iteration = st_iteration_.load(std::memory_order_relaxed);
  s.iteration_ms = st_iteration_ms_.load(std::memory_order_relaxed);
  s.tree_ms = st_tree_ms_.load(std::memory_order_relaxed);
  s.repulsion_ms = st_repulsion_ms_.load(std::memory_order_relaxed);
  s.integrate_ms = st_integrate_ms_.load(std::memory_order_relaxed);
  s.mean_displacement = st_mean_disp_.load(std::memory_order_relaxed);
  s.temperature = st_temperature_.load(std::memory_order_relaxed);
  s.converged = st_converged_.load(std::memory_order_relaxed);
  s.paused = paused_.load(std::memory_order_relaxed);
  s.running = running_.load(std::memory_order_relaxed);
  return s;
}

void LayoutEngine::publish_now(const IterationStats* last) {
  if (last) {
    publish_iteration(*last);
    return;
  }
  PositionSnapshot& back = snapshots_.back();
  solver_.write_packed_xyz(back.xyz.data());
  back.iteration = solver_.iteration();
  snapshots_.publish();
  st_iteration_.store(solver_.iteration(), std::memory_order_relaxed);
  st_temperature_.store(solver_.temperature(), std::memory_order_relaxed);
  st_converged_.store(solver_.converged(), std::memory_order_relaxed);
}

void LayoutEngine::publish_iteration(const IterationStats& s) {
  PositionSnapshot& back = snapshots_.back();
  solver_.write_packed_xyz(back.xyz.data());
  back.iteration = s.iteration;
  snapshots_.publish();
  st_iteration_.store(s.iteration, std::memory_order_relaxed);
  st_iteration_ms_.store(static_cast<float>(s.total_ms), std::memory_order_relaxed);
  st_tree_ms_.store(static_cast<float>(s.tree_ms), std::memory_order_relaxed);
  st_repulsion_ms_.store(static_cast<float>(s.repulsion_ms), std::memory_order_relaxed);
  st_integrate_ms_.store(static_cast<float>(s.integrate_ms), std::memory_order_relaxed);
  st_mean_disp_.store(static_cast<float>(s.mean_displacement), std::memory_order_relaxed);
  st_temperature_.store(s.temperature, std::memory_order_relaxed);
  st_converged_.store(s.converged, std::memory_order_relaxed);
}

void LayoutEngine::thread_main(std::stop_token st) {
  while (!st.stop_requested()) {
    // Load the control counter BEFORE inspecting state: any change made after
    // this load bumps the counter, so the wait below returns immediately.
    const std::uint32_t ctl = control_.load(std::memory_order_acquire);
    if (reheat_requested_.exchange(false, std::memory_order_relaxed)) {
      solver_.reheat(solver_.params().initial_temperature * 0.25f);
      st_converged_.store(false, std::memory_order_relaxed);
    }
    if (paused_.load(std::memory_order_relaxed) || solver_.converged()) {
      control_.wait(ctl, std::memory_order_acquire);  // idle: zero CPU until woken
      continue;
    }
    publish_iteration(solver_.step());
  }
}

}  // namespace ftv
