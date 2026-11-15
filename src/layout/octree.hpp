#pragma once
// Barnes-Hut octree rebuilt every iteration:
//   1. parallel bounding-box reduction,
//   2. parallel Morton codes,
//   3. parallel stable LSD radix sort (3 x 10-bit passes),
//   4. parallel gather of bodies into sorted order,
//   5. parallel subtree node counting per depth-3 cell, serial top levels,
//      parallel pre-order subtree construction with bottom-up aggregation,
//   6. interaction groups: maximal single-hub subtrees of <= kGroupSize bodies.
//
// Nodes are stored SoA in pre-order with skip pointers, so traversal is
// stackless and walks memory forward. The traversal-hot fields are grouped so
// that one visit touches as few cache lines as possible: the centre of charge
// and total charge form one 16-byte field (`com`), hub id / depth / leaf flag
// are packed in one 32-bit `meta` word, and a cell's squared size comes from a
// per-depth table instead of a per-node array.
//
// Repulsion is evaluated per group: one traversal builds an interaction list
// shared by all bodies of the group (opening test against the group's tight
// bounding box, which is conservative; cells containing the group are always
// opened), then a SIMD kernel evaluates the list for 8 bodies at a time.
//
// Hub-aware repulsion: a cell whose bodies all share one hub carries that hub
// id; mixed cells always open, so the same-hub (x1) / cross-hub (x0.05)
// scaling is applied exactly per cell or per body.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/aligned_allocator.hpp"
#include "core/thread_pool.hpp"

namespace ftv {

struct RepulsionParams {
  float theta = 0.8f;
  float softening = 1.0f;
  float cross_hub_scale = 0.05f;
};

class Octree {
 public:
  static constexpr std::uint16_t kMixedHub = 0xFFFF;
  static constexpr unsigned kMaxDepth = 10;  // == kMortonLevels

  Octree() noexcept : use_avx_(cpu_supports_avx()) {}

  // True when the 8-wide AVX kernel is used (built in, or detected at run
  // time on x86-64). Results are identical either way.
  [[nodiscard]] bool uses_avx() const noexcept { return use_avx_; }
  static bool cpu_supports_avx() noexcept;

  void build(const float* x, const float* y, const float* z, const float* q, const std::uint16_t* hub, std::size_t n,
             unsigned leafSize, ThreadPool& pool);

  // Writes the repulsive field for every body in SORTED order (slot k is body
  // order()[k]). Each leaf writes only its own contiguous slot range.
  void compute_repulsion(const RepulsionParams& params, float* outX, float* outY, float* outZ, ThreadPool& pool);

  [[nodiscard]] std::span<const std::uint32_t> order() const noexcept { return {order_.data(), n_}; }
  [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }
  [[nodiscard]] std::size_t leaf_count() const noexcept { return leaf_count_; }
  [[nodiscard]] std::size_t group_count() const noexcept { return group_count_; }
  [[nodiscard]] std::size_t body_count() const noexcept { return n_; }
  // Body-cell plus body-body interactions evaluated by the last
  // compute_repulsion() call (list length summed over bodies).
  [[nodiscard]] std::uint64_t last_interactions() const noexcept;

 private:
  // Bodies per interaction group (one traversal + one shared list).
  static constexpr std::uint32_t kGroupSize = 64;

  struct Task {
    std::uint32_t begin, end, offset, level;
  };
  // Centre of charge (x, y, z) and total charge q of a node.
  struct alignas(16) CellCom {
    float x, y, z, q;
  };
  static constexpr std::uint32_t kMetaHubMask = 0xFFFFu;
  static constexpr unsigned kMetaLevelShift = 16;
  static constexpr std::uint32_t kMetaLeaf = 1u << 24;
  struct alignas(kCacheLine) Scratch {
    AlignedVector<float> x, y, z, q;
    std::size_t size = 0;
    std::uint64_t interactions = 0;  // per participant: no sharing, summed after the pass
    void clear() noexcept { size = 0; }
    void push(float px, float py, float pz, float pq) {
      if (size == x.size()) {
        const std::size_t cap = x.empty() ? 1024 : x.size() * 2;  // warm-up growth only
        x.resize(cap);
        y.resize(cap);
        z.resize(cap);
        q.resize(cap);
      }
      x[size] = px;
      y[size] = py;
      z[size] = pz;
      q[size] = pq;
      ++size;
    }
  };

  void compute_bounds(const float* x, const float* y, const float* z, ThreadPool& pool);
  void radix_sort(ThreadPool& pool);
  std::uint32_t count_subtree(std::uint32_t b, std::uint32_t e, unsigned level) const noexcept;
  std::uint32_t build_subtree(std::uint32_t b, std::uint32_t e, unsigned level, std::uint32_t idx) noexcept;
  std::uint32_t build_top(std::uint32_t b, std::uint32_t e, unsigned level, std::uint32_t idx);
  void aggregate_internal(std::uint32_t idx) noexcept;
  void make_leaf(std::uint32_t idx, std::uint32_t b, std::uint32_t e, unsigned level) noexcept;
  std::uint32_t child_range_end(std::uint32_t b, std::uint32_t e, unsigned level, unsigned digit) const noexcept;
  void traverse(const float* bmin, const float* bmax, std::uint16_t hub, std::uint32_t owner,
                const RepulsionParams& p, Scratch& s) const;
  void evaluate(const Scratch& s, std::uint32_t first, std::uint32_t count, float eps2, float* outX, float* outY,
                float* outZ) const noexcept;
  void build_groups();
  void ensure_node_capacity(std::size_t n);
  [[nodiscard]] std::uint32_t subtree_body_end(std::uint32_t node) const noexcept {
    const std::uint32_t next = nskip_[node];
    return next < node_count_ ? nfirst_[next] : static_cast<std::uint32_t>(n_);
  }

  std::size_t n_ = 0;
  unsigned leaf_size_ = 16;
  float min_[3] = {0, 0, 0};
  float side_ = 1.0f;

  AlignedVector<float> chunk_bounds_;  // 6 floats per chunk
  AlignedVector<std::uint32_t> codes_, codes_tmp_;
  AlignedVector<std::uint32_t> order_, order_tmp_;
  AlignedVector<std::uint32_t> histograms_;

  // Bodies in sorted order.
  AlignedVector<float> bx_, by_, bz_, bq_;
  AlignedVector<std::uint16_t> bhub_;

  // Nodes (SoA, pre-order).
  std::size_t node_count_ = 0;
  std::size_t leaf_count_ = 0;
  AlignedVector<CellCom> ncom_;          // centre of charge + total charge
  AlignedVector<std::uint32_t> nmeta_;   // hub | depth << 16 | leaf flag
  AlignedVector<std::uint32_t> nskip_;   // next node after this subtree
  AlignedVector<std::uint32_t> nfirst_;  // first body (sorted order) of the subtree
  AlignedVector<std::uint32_t> ncount_;  // bodies in a leaf (0 for internal nodes)
  float level_size2_[kMaxDepth + 1] = {};

  // Interaction groups (SoA).
  std::size_t group_count_ = 0;
  AlignedVector<std::uint32_t> gnode_, gfirst_, gcount_;

  // Top-level decomposition.
  static constexpr unsigned kTopLevel = 3;
  static constexpr unsigned kTopCells = 1u << (3 * kTopLevel);
  std::vector<std::uint32_t> cell_begin_;
  std::vector<std::uint32_t> cell_count_;
  std::vector<Task> tasks_;
  std::vector<std::uint32_t> top_internal_;  // top-level internal nodes, post-order

  std::vector<Scratch> scratch_;
  bool use_avx_ = false;
};

// O(N^2) reference used by the unit tests (original body order).
void brute_force_repulsion(const float* x, const float* y, const float* z, const float* q, const std::uint16_t* hub,
                           std::size_t n, const RepulsionParams& params, float* outX, float* outY, float* outZ);

}  // namespace ftv
