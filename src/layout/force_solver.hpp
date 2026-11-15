#pragma once
// Constrained, hub-anchored force-directed solver (one iteration = step()).
//
// Per iteration:
//   1. octree build on current positions (parallel; see octree.hpp)
//   2. Barnes-Hut repulsion per interaction group (a single-hub subtree of
//      <= 64 bodies sharing one interaction list) -> sorted-slot arrays
//   3. inverse permutation, each thread owning a contiguous range of bodies
//   4. springs + hub anchor + core gravity + velocity Verlet integration per
//      body range -> next-position buffer (positions are double-buffered, so
//      spring reads of neighbours never race with writes)
// Every parallel pass writes only its own contiguous output range, and every
// reduction is combined per fixed chunk in chunk order: results are
// bit-identical across runs and thread counts.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

#include "core/aligned_allocator.hpp"
#include "core/thread_pool.hpp"
#include "layout/layout_params.hpp"
#include "layout/octree.hpp"
#include "model/graph.hpp"
#include "model/hub_taxonomy.hpp"

namespace ftv {

struct IterationStats {
  std::uint64_t iteration = 0;
  double total_ms = 0.0;
  double tree_ms = 0.0;
  double repulsion_ms = 0.0;
  double integrate_ms = 0.0;
  double mean_displacement = 0.0;
  float temperature = 0.0f;
  bool converged = false;
};

class ForceSolver {
 public:
  ForceSolver(const Graph& graph, const HubTaxonomy& taxonomy, const LayoutParams& params, ThreadPool& pool);

  // Deterministic initial placement; resets velocities, temperature and the
  // convergence counter.
  void reset();
  IterationStats step();
  // Restarts annealing from the current positions (used for "new data").
  void reheat(float temperature);

  [[nodiscard]] bool converged() const noexcept { return converged_; }
  [[nodiscard]] float temperature() const noexcept { return temperature_; }
  [[nodiscard]] std::uint64_t iteration() const noexcept { return iteration_; }
  [[nodiscard]] std::size_t node_count() const noexcept { return n_; }
  // Position of graph node `node` (graph / dense-index order).
  [[nodiscard]] glm::vec3 position(std::size_t node) const noexcept {
    const std::uint32_t a = rank_[node];
    return {x_[a], y_[a], z_[a]};
  }
  [[nodiscard]] const LayoutParams& params() const noexcept { return params_; }
  // BH body-cell + body-body interactions per body in the last iteration.
  [[nodiscard]] double interactions_per_body() const noexcept {
    return n_ ? static_cast<double>(tree_.last_interactions()) / static_cast<double>(n_) : 0.0;
  }
  [[nodiscard]] bool simd_avx() const noexcept { return tree_.uses_avx(); }

  // XXH3 over the position arrays (bitwise determinism check).
  [[nodiscard]] std::uint64_t checksum() const noexcept;

  // Writes interleaved xyz (3 floats per node, graph order) for GPU upload.
  void write_packed_xyz(float* dst) const;

 private:
  void build_order(const Graph& graph);
  void build_springs(const Graph& graph);

  ThreadPool& pool_;
  LayoutParams params_;
  std::size_t n_ = 0;
  std::uint64_t iteration_ = 0;
  float temperature_ = 0.0f;
  unsigned calm_iterations_ = 0;
  bool converged_ = false;
  bool have_prev_accel_ = false;

  // Internal node order: hubs in sequence, breadth-first over intra-hub
  // edges inside each hub (a Cuthill-McKee style bandwidth reduction).
  // Counterparties get nearby indices, so the spring pass gathers neighbour
  // positions from a small, cache-resident window instead of the whole
  // array. perm_: internal -> graph index; rank_: graph -> internal.
  AlignedVector<std::uint32_t> perm_, rank_;

  // Per-node state (SoA, 64-byte aligned, internal order).
  AlignedVector<float> x_, y_, z_;        // current positions
  AlignedVector<float> nx_, ny_, nz_;     // next positions (double buffer)
  AlignedVector<float> vx_, vy_, vz_;
  AlignedVector<float> pax_, pay_, paz_;  // previous accelerations (Verlet)
  AlignedVector<float> charge_;
  AlignedVector<std::uint16_t> hub_;
  AlignedVector<std::uint64_t> key_;

  // Per-hub constants indexed by hub id.
  std::vector<float> hub_ax_, hub_ay_, hub_az_, hub_r_, hub_k_, hub_core_;

  // Springs as CSR incidences (each node sums its own springs: no atomics).
  AlignedVector<std::uint32_t> spring_offset_;
  AlignedVector<std::uint32_t> spring_other_;
  AlignedVector<float> spring_rest_;
  AlignedVector<float> spring_k_;

  // Octree and scratch.
  Octree tree_;
  AlignedVector<float> rep_x_, rep_y_, rep_z_;  // sorted-slot repulsion
  AlignedVector<std::uint32_t> inverse_;        // body -> sorted slot
  AlignedVector<double> chunk_disp_;            // per-chunk displacement sums
};

}  // namespace ftv
