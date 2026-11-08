#include "layout/octree.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "layout/morton.hpp"

// SIMD kernels for the interaction-list evaluation. All variants run the same
// IEEE operations in the same order per lane (sqrt and div are correctly
// rounded, no FMA), so they produce bit-identical forces:
//   * AVX   - 8 lanes per register. Compiled in when the build targets AVX
//             (FTVIZ_NATIVE / -mavx / /arch:AVX2), otherwise compiled with a
//             function-level target attribute and selected at run time when
//             the CPU and OS support it;
//   * SSE2  - 2 x 4 lanes; baseline on every x86-64 CPU;
//   * scalar- portable fallback for other architectures.
#if defined(__x86_64__) || defined(_M_X64)
#define FTV_BH_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define FTV_TARGET_AVX
#else
#define FTV_TARGET_AVX __attribute__((target("avx")))
#endif
#endif

namespace ftv {

namespace {
constexpr std::size_t kBoundsGrain = 16384;
constexpr std::size_t kSortGrain = 65536;
constexpr unsigned kRadixBits = 10;
constexpr unsigned kRadixBuckets = 1u << kRadixBits;
static_assert(Octree::kMaxDepth == kMortonLevels);
}  // namespace

void Octree::compute_bounds(const float* x, const float* y, const float* z, ThreadPool& pool) {
  const std::size_t chunks = ThreadPool::chunk_count(n_, kBoundsGrain);
  if (chunk_bounds_.size() < chunks * 6) chunk_bounds_.resize(chunks * 6);
  pool.parallel_for(n_, kBoundsGrain, [&](std::size_t b, std::size_t e, unsigned) {
    float mn[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float mx[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    for (std::size_t i = b; i < e; ++i) {
      mn[0] = std::min(mn[0], x[i]);
      mn[1] = std::min(mn[1], y[i]);
      mn[2] = std::min(mn[2], z[i]);
      mx[0] = std::max(mx[0], x[i]);
      mx[1] = std::max(mx[1], y[i]);
      mx[2] = std::max(mx[2], z[i]);
    }
    float* out = &chunk_bounds_[(b / kBoundsGrain) * 6];  // each chunk owns its own 6 floats
    for (int a = 0; a < 3; ++a) {
      out[a] = mn[a];
      out[3 + a] = mx[a];
    }
  });
  float mn[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
  float mx[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};
  for (std::size_t c = 0; c < chunks; ++c) {
    for (int a = 0; a < 3; ++a) {
      mn[a] = std::min(mn[a], chunk_bounds_[c * 6 + a]);
      mx[a] = std::max(mx[a], chunk_bounds_[c * 6 + 3 + a]);
    }
  }
  float extent = 0.0f;
  for (int a = 0; a < 3; ++a) {
    if (!(mn[a] <= mx[a])) {  // n == 0 or NaN: fall back to a unit cube
      mn[a] = 0.0f;
      mx[a] = 1.0f;
    }
    extent = std::max(extent, mx[a] - mn[a]);
    min_[a] = mn[a];
  }
  side_ = extent * 1.0001f + 1e-3f;
}

void Octree::radix_sort(ThreadPool& pool) {
  const std::size_t chunks = ThreadPool::chunk_count(n_, kSortGrain);
  if (histograms_.size() < chunks * kRadixBuckets) histograms_.resize(chunks * kRadixBuckets);
  for (unsigned pass = 0; pass < 3; ++pass) {
    const unsigned shift = pass * kRadixBits;
    // Per-chunk histograms: chunk c writes only row c.
    pool.parallel_for(n_, kSortGrain, [&](std::size_t b, std::size_t e, unsigned) {
      std::uint32_t* h = &histograms_[(b / kSortGrain) * kRadixBuckets];
      std::fill(h, h + kRadixBuckets, 0u);
      for (std::size_t i = b; i < e; ++i) ++h[(codes_[i] >> shift) & (kRadixBuckets - 1)];
    });
    // Exclusive prefix in (digit, chunk) order => stable scatter positions.
    std::uint32_t running = 0;
    for (unsigned d = 0; d < kRadixBuckets; ++d) {
      for (std::size_t c = 0; c < chunks; ++c) {
        const std::uint32_t t = histograms_[c * kRadixBuckets + d];
        histograms_[c * kRadixBuckets + d] = running;
        running += t;
      }
    }
    // Scatter: chunk c writes to its reserved destination runs only.
    pool.parallel_for(n_, kSortGrain, [&](std::size_t b, std::size_t e, unsigned) {
      std::uint32_t* off = &histograms_[(b / kSortGrain) * kRadixBuckets];
      for (std::size_t i = b; i < e; ++i) {
        const std::uint32_t k = codes_[i];
        const std::uint32_t pos = off[(k >> shift) & (kRadixBuckets - 1)]++;
        codes_tmp_[pos] = k;
        order_tmp_[pos] = order_[i];
      }
    });
    codes_.swap(codes_tmp_);
    order_.swap(order_tmp_);
  }
}

void Octree::ensure_node_capacity(std::size_t n) {
  if (ncom_.size() >= n) return;
  const std::size_t cap = n + n / 4 + 64;  // grows during warm-up only
  ncom_.resize(cap);
  nmeta_.resize(cap);
  nskip_.resize(cap);
  nfirst_.resize(cap);
  ncount_.resize(cap);
}

std::uint32_t Octree::child_range_end(std::uint32_t b, std::uint32_t e, unsigned level, unsigned digit) const noexcept {
  const std::uint32_t* first = codes_.data() + b;
  const std::uint32_t* last = codes_.data() + e;
  const std::uint32_t* it =
      std::partition_point(first, last, [level, digit](std::uint32_t c) { return morton_digit(c, level) <= digit; });
  return static_cast<std::uint32_t>(it - codes_.data());
}

std::uint32_t Octree::count_subtree(std::uint32_t b, std::uint32_t e, unsigned level) const noexcept {
  if (e - b <= leaf_size_ || level >= kMortonLevels) return 1;
  std::uint32_t total = 1;
  std::uint32_t cb = b;
  for (unsigned d = 0; d < 8 && cb < e; ++d) {
    const std::uint32_t ce = child_range_end(cb, e, level, d);
    if (ce > cb) total += count_subtree(cb, ce, level + 1);
    cb = ce;
  }
  return total;
}

void Octree::make_leaf(std::uint32_t idx, std::uint32_t b, std::uint32_t e, unsigned level) noexcept {
  double q = 0, cx = 0, cy = 0, cz = 0;
  std::uint16_t hub = bhub_[b];
  for (std::uint32_t k = b; k < e; ++k) {
    const double w = bq_[k];
    q += w;
    cx += w * bx_[k];
    cy += w * by_[k];
    cz += w * bz_[k];
    if (bhub_[k] != hub) hub = kMixedHub;
  }
  const double inv = q > 0 ? 1.0 / q : 0.0;
  ncom_[idx] = CellCom{static_cast<float>(cx * inv), static_cast<float>(cy * inv), static_cast<float>(cz * inv),
                       static_cast<float>(q)};
  nmeta_[idx] = static_cast<std::uint32_t>(hub) | (level << kMetaLevelShift) | kMetaLeaf;
  nfirst_[idx] = b;
  ncount_[idx] = e - b;
  nskip_[idx] = idx + 1;
}

void Octree::aggregate_internal(std::uint32_t idx) noexcept {
  double q = 0, cx = 0, cy = 0, cz = 0;
  std::uint32_t hub = 0;
  bool first = true;
  const std::uint32_t end = nskip_[idx];
  for (std::uint32_t c = idx + 1; c < end; c = nskip_[c]) {
    const CellCom& cc = ncom_[c];
    const double w = cc.q;
    q += w;
    cx += w * cc.x;
    cy += w * cc.y;
    cz += w * cc.z;
    const std::uint32_t h = nmeta_[c] & kMetaHubMask;
    if (first) {
      hub = h;
      first = false;
    } else if (h != hub) {
      hub = kMixedHub;
    }
  }
  const double inv = q > 0 ? 1.0 / q : 0.0;
  ncom_[idx] = CellCom{static_cast<float>(cx * inv), static_cast<float>(cy * inv), static_cast<float>(cz * inv),
                       static_cast<float>(q)};
  nmeta_[idx] = (nmeta_[idx] & ~kMetaHubMask) | hub;
}

std::uint32_t Octree::build_subtree(std::uint32_t b, std::uint32_t e, unsigned level, std::uint32_t idx) noexcept {
  if (e - b <= leaf_size_ || level >= kMortonLevels) {
    make_leaf(idx, b, e, level);
    return idx + 1;
  }
  std::uint32_t next = idx + 1;
  std::uint32_t cb = b;
  for (unsigned d = 0; d < 8 && cb < e; ++d) {
    const std::uint32_t ce = child_range_end(cb, e, level, d);
    if (ce > cb) next = build_subtree(cb, ce, level + 1, next);
    cb = ce;
  }
  nmeta_[idx] = level << kMetaLevelShift;
  nfirst_[idx] = b;
  ncount_[idx] = 0;
  nskip_[idx] = next;
  aggregate_internal(idx);  // bottom-up: children are complete
  return next;
}

std::uint32_t Octree::build_top(std::uint32_t b, std::uint32_t e, unsigned level, std::uint32_t idx) {
  if (level == kTopLevel) {
    tasks_.push_back(Task{b, e, idx, level});
    return idx + cell_count_[codes_[b] >> (3 * (kMortonLevels - kTopLevel))];
  }
  if (e - b <= leaf_size_) {
    make_leaf(idx, b, e, level);
    return idx + 1;
  }
  std::uint32_t next = idx + 1;
  std::uint32_t cb = b;
  for (unsigned d = 0; d < 8 && cb < e; ++d) {
    const std::uint32_t ce = child_range_end(cb, e, level, d);
    if (ce > cb) next = build_top(cb, ce, level + 1, next);
    cb = ce;
  }
  nmeta_[idx] = level << kMetaLevelShift;
  nfirst_[idx] = b;
  ncount_[idx] = 0;
  nskip_[idx] = next;
  top_internal_.push_back(idx);
  return next;
}

void Octree::build(const float* x, const float* y, const float* z, const float* q, const std::uint16_t* hub,
                   std::size_t n, unsigned leafSize, ThreadPool& pool) {
  n_ = n;
  leaf_size_ = std::max(1u, leafSize);
  if (codes_.size() < n) {
    codes_.resize(n);
    codes_tmp_.resize(n);
    order_.resize(n);
    order_tmp_.resize(n);
    bx_.resize(n);
    by_.resize(n);
    bz_.resize(n);
    bq_.resize(n);
    bhub_.resize(n);
  }
  node_count_ = 0;
  leaf_count_ = 0;
  group_count_ = 0;
  if (n == 0) return;

  compute_bounds(x, y, z, pool);
  for (unsigned l = 0; l <= kMaxDepth; ++l) {
    const float s = side_ / static_cast<float>(1u << l);
    level_size2_[l] = s * s;
  }
  const float inv = static_cast<float>(1u << kMortonBitsPerAxis) / side_;
  const float mx = min_[0], my = min_[1], mz = min_[2];
  pool.parallel_for(n, 16384, [&](std::size_t b, std::size_t e, unsigned) {
    constexpr float kMaxQ = static_cast<float>((1u << kMortonBitsPerAxis) - 1);
    const auto quant = [](float t) {  // kMaxQ is constexpr: no capture needed
      if (!(t >= 0.0f)) t = 0.0f;  // also catches NaN
      if (t > kMaxQ) t = kMaxQ;
      return static_cast<std::uint32_t>(t);
    };
    for (std::size_t i = b; i < e; ++i) {
      codes_[i] = morton3(quant((x[i] - mx) * inv), quant((y[i] - my) * inv), quant((z[i] - mz) * inv));
      order_[i] = static_cast<std::uint32_t>(i);
    }
  });
  radix_sort(pool);
  pool.parallel_for(n, 16384, [&](std::size_t b, std::size_t e, unsigned) {
    for (std::size_t k = b; k < e; ++k) {
      const std::uint32_t i = order_[k];
      bx_[k] = x[i];
      by_[k] = y[i];
      bz_[k] = z[i];
      bq_[k] = q[i];
      bhub_[k] = hub[i];
    }
  });

  const auto N = static_cast<std::uint32_t>(n);
  if (n <= leaf_size_) {
    ensure_node_capacity(1);
    make_leaf(0, 0, N, 0);
    node_count_ = 1;
    build_groups();
    return;
  }

  // Ranges of the 512 depth-3 cells (codes are sorted, so cells are runs).
  cell_begin_.resize(kTopCells + 1);
  cell_count_.assign(kTopCells, 0);
  constexpr unsigned kTopShift = 3 * (kMortonLevels - kTopLevel);
  for (unsigned c = 0; c <= kTopCells; ++c) {
    cell_begin_[c] = static_cast<std::uint32_t>(
        std::lower_bound(codes_.data(), codes_.data() + n, c << kTopShift) - codes_.data());
  }
  pool.parallel_for(kTopCells, 8, [&](std::size_t b, std::size_t e, unsigned) {
    for (std::size_t c = b; c < e; ++c) {
      const std::uint32_t cb = cell_begin_[c], ce = cell_begin_[c + 1];
      cell_count_[c] = ce > cb ? count_subtree(cb, ce, kTopLevel) : 0u;
    }
  });

  // Exact node total via a dry run of the top levels, then build for real.
  tasks_.clear();
  top_internal_.clear();
  {
    // Dry run: count top nodes without writing node arrays.
    struct Counter {
      const Octree* t;
      std::uint32_t run(std::uint32_t b, std::uint32_t e, unsigned level) const {
        if (level == kTopLevel) return t->cell_count_[t->codes_[b] >> kTopShift];
        if (e - b <= t->leaf_size_) return 1;
        std::uint32_t total = 1, cb = b;
        for (unsigned d = 0; d < 8 && cb < e; ++d) {
          const std::uint32_t ce = t->child_range_end(cb, e, level, d);
          if (ce > cb) total += run(cb, ce, level + 1);
          cb = ce;
        }
        return total;
      }
    };
    ensure_node_capacity(Counter{this}.run(0, N, 0));
  }
  node_count_ = build_top(0, N, 0, 0);

  pool.parallel_for(tasks_.size(), 1, [&](std::size_t b, std::size_t e, unsigned) {
    for (std::size_t t = b; t < e; ++t) {
      const Task& task = tasks_[t];
      build_subtree(task.begin, task.end, task.level, task.offset);  // writes [offset, offset+count)
    }
  });
  // Top internal nodes bottom-up: build_top() records them in post-order
  // (after recursing into the children), so a forward walk aggregates every
  // child before its parent.
  for (const std::uint32_t idx : top_internal_) aggregate_internal(idx);
  build_groups();
}

void Octree::build_groups() {
  // Pre-order walk that stops at the first node whose subtree is single-hub
  // and holds <= kGroupSize bodies (or at a leaf): O(groups x depth).
  if (gnode_.size() < n_ + 1) {
    gnode_.resize(n_ + 1);
    gfirst_.resize(n_ + 1);
    gcount_.resize(n_ + 1);
  }
  std::size_t g = 0, leaves = 0;
  const auto end = static_cast<std::uint32_t>(node_count_);
  for (std::uint32_t nd = 0; nd < end;) {
    const std::uint32_t meta = nmeta_[nd];
    const bool leaf = (meta & kMetaLeaf) != 0;
    const bool mixed = (meta & kMetaHubMask) == kMixedHub;
    const std::uint32_t first = nfirst_[nd];
    const std::uint32_t count = subtree_body_end(nd) - first;
    if ((!mixed && count <= kGroupSize) || leaf) {
      gnode_[g] = nd;
      gfirst_[g] = first;
      gcount_[g] = count;
      ++g;
      nd = nskip_[nd];
    } else {
      ++nd;
    }
  }
  for (std::uint32_t nd = 0; nd < end; ++nd) leaves += (nmeta_[nd] & kMetaLeaf) ? 1u : 0u;
  group_count_ = g;
  leaf_count_ = leaves;
}

void Octree::traverse(const float* bmin, const float* bmax, std::uint16_t hub, std::uint32_t owner,
                      const RepulsionParams& p, Scratch& s) const {
  const float theta2 = p.theta * p.theta;
  const float cross = p.cross_hub_scale;
  std::uint32_t n = 0;
  const auto end = static_cast<std::uint32_t>(node_count_);
  while (n < end) {
    const std::uint32_t meta = nmeta_[n];
    const std::uint32_t h = meta & kMetaHubMask;
    // A cell that contains the group (owner lies in its pre-order subtree
    // range) is always opened, so no body ever interacts with an aggregate
    // that includes itself. Mixed-hub cells always open.
    const bool containsOwner = n <= owner && owner < nskip_[n];
    if (h != kMixedHub && !containsOwner) {
      // Distance from the group's bounding box to the cell's centre of
      // charge (zero if inside), compared with the cell size: s < theta * d.
      const CellCom c = ncom_[n];
      const float dx = std::max(0.0f, std::max(bmin[0] - c.x, c.x - bmax[0]));
      const float dy = std::max(0.0f, std::max(bmin[1] - c.y, c.y - bmax[1]));
      const float dz = std::max(0.0f, std::max(bmin[2] - c.z, c.z - bmax[2]));
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (level_size2_[(meta >> kMetaLevelShift) & 0xFFu] < theta2 * d2) {
        s.push(c.x, c.y, c.z, c.q * (h == hub ? 1.0f : cross));
        n = nskip_[n];
        continue;
      }
    }
    if (meta & kMetaLeaf) {
      const std::uint32_t f = nfirst_[n];
      const std::uint32_t cnt = ncount_[n];
      for (std::uint32_t k = f; k < f + cnt; ++k) {
        s.push(bx_[k], by_[k], bz_[k], bq_[k] * (bhub_[k] == hub ? 1.0f : cross));
      }
      n = nskip_[n];
    } else {
      ++n;
    }
  }
}

namespace {

// Eight bodies (lanes) evaluated against one interaction list.
struct alignas(32) LaneBlock {
  float px[8], py[8], pz[8];
  float ax[8], ay[8], az[8];
};

// Field per list entry j (Plummer-softened inverse square):
//   d = p - s_j,  r2 = |d|^2 + eps^2,  F += q_j * d / (r2 * sqrt(r2))
// Self-interaction contributes exactly zero (d == 0).
[[maybe_unused]] void kernel_scalar(LaneBlock& b, const float* sx, const float* sy, const float* sz, const float* sq,
                                    std::size_t m, float eps2) noexcept {
  for (int l = 0; l < 8; ++l) b.ax[l] = b.ay[l] = b.az[l] = 0.0f;
  for (std::size_t j = 0; j < m; ++j) {
    for (int l = 0; l < 8; ++l) {
      const float dx = b.px[l] - sx[j];
      const float dy = b.py[l] - sy[j];
      const float dz = b.pz[l] - sz[j];
      const float r2 = dx * dx + dy * dy + dz * dz + eps2;
      const float w = sq[j] / (r2 * std::sqrt(r2));
      b.ax[l] += w * dx;
      b.ay[l] += w * dy;
      b.az[l] += w * dz;
    }
  }
}

#if defined(FTV_BH_X86)
void kernel_sse2(LaneBlock& b, const float* sx, const float* sy, const float* sz, const float* sq, std::size_t m,
                 float eps2) noexcept {
  const __m128 pxl = _mm_load_ps(b.px), pxh = _mm_load_ps(b.px + 4);
  const __m128 pyl = _mm_load_ps(b.py), pyh = _mm_load_ps(b.py + 4);
  const __m128 pzl = _mm_load_ps(b.pz), pzh = _mm_load_ps(b.pz + 4);
  const __m128 ve = _mm_set1_ps(eps2);
  __m128 axl = _mm_setzero_ps(), axh = _mm_setzero_ps();
  __m128 ayl = _mm_setzero_ps(), ayh = _mm_setzero_ps();
  __m128 azl = _mm_setzero_ps(), azh = _mm_setzero_ps();
  for (std::size_t j = 0; j < m; ++j) {
    const __m128 qx = _mm_set1_ps(sx[j]), qy = _mm_set1_ps(sy[j]), qz = _mm_set1_ps(sz[j]);
    const __m128 qq = _mm_set1_ps(sq[j]);
    const __m128 dxl = _mm_sub_ps(pxl, qx), dxh = _mm_sub_ps(pxh, qx);
    const __m128 dyl = _mm_sub_ps(pyl, qy), dyh = _mm_sub_ps(pyh, qy);
    const __m128 dzl = _mm_sub_ps(pzl, qz), dzh = _mm_sub_ps(pzh, qz);
    const __m128 r2l =
        _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dxl, dxl), _mm_mul_ps(dyl, dyl)), _mm_mul_ps(dzl, dzl)), ve);
    const __m128 r2h =
        _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dxh, dxh), _mm_mul_ps(dyh, dyh)), _mm_mul_ps(dzh, dzh)), ve);
    const __m128 wl = _mm_div_ps(qq, _mm_mul_ps(r2l, _mm_sqrt_ps(r2l)));
    const __m128 wh = _mm_div_ps(qq, _mm_mul_ps(r2h, _mm_sqrt_ps(r2h)));
    axl = _mm_add_ps(axl, _mm_mul_ps(wl, dxl));
    axh = _mm_add_ps(axh, _mm_mul_ps(wh, dxh));
    ayl = _mm_add_ps(ayl, _mm_mul_ps(wl, dyl));
    ayh = _mm_add_ps(ayh, _mm_mul_ps(wh, dyh));
    azl = _mm_add_ps(azl, _mm_mul_ps(wl, dzl));
    azh = _mm_add_ps(azh, _mm_mul_ps(wh, dzh));
  }
  _mm_store_ps(b.ax, axl);
  _mm_store_ps(b.ax + 4, axh);
  _mm_store_ps(b.ay, ayl);
  _mm_store_ps(b.ay + 4, ayh);
  _mm_store_ps(b.az, azl);
  _mm_store_ps(b.az + 4, azh);
}

FTV_TARGET_AVX void kernel_avx(LaneBlock& b, const float* sx, const float* sy, const float* sz, const float* sq,
                               std::size_t m, float eps2) noexcept {
  const __m256 vpx = _mm256_load_ps(b.px), vpy = _mm256_load_ps(b.py), vpz = _mm256_load_ps(b.pz);
  const __m256 ve = _mm256_set1_ps(eps2);
  __m256 vax = _mm256_setzero_ps(), vay = _mm256_setzero_ps(), vaz = _mm256_setzero_ps();
  for (std::size_t j = 0; j < m; ++j) {
    const __m256 dx = _mm256_sub_ps(vpx, _mm256_set1_ps(sx[j]));
    const __m256 dy = _mm256_sub_ps(vpy, _mm256_set1_ps(sy[j]));
    const __m256 dz = _mm256_sub_ps(vpz, _mm256_set1_ps(sz[j]));
    const __m256 r2 = _mm256_add_ps(
        _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy)), _mm256_mul_ps(dz, dz)), ve);
    const __m256 w = _mm256_div_ps(_mm256_set1_ps(sq[j]), _mm256_mul_ps(r2, _mm256_sqrt_ps(r2)));
    vax = _mm256_add_ps(vax, _mm256_mul_ps(w, dx));
    vay = _mm256_add_ps(vay, _mm256_mul_ps(w, dy));
    vaz = _mm256_add_ps(vaz, _mm256_mul_ps(w, dz));
  }
  _mm256_store_ps(b.ax, vax);
  _mm256_store_ps(b.ay, vay);
  _mm256_store_ps(b.az, vaz);
  _mm256_zeroupper();  // avoid AVX-SSE transition penalties in the caller
}
#endif

}  // namespace

bool Octree::cpu_supports_avx() noexcept {
#if defined(__AVX__)
  return true;
#elif defined(FTV_BH_X86) && defined(_MSC_VER) && !defined(__clang__)
  int r[4] = {0, 0, 0, 0};
  __cpuid(r, 1);
  const bool osxsave = (r[2] & (1 << 27)) != 0;
  const bool avx = (r[2] & (1 << 28)) != 0;
  return osxsave && avx && (_xgetbv(0) & 0x6) == 0x6;  // OS saves XMM and YMM state
#elif defined(FTV_BH_X86)
  return __builtin_cpu_supports("avx");  // checks CPUID and OS (XCR0) support
#else
  return false;
#endif
}

// Evaluates the shared interaction list for bodies [first, first + count),
// 8 bodies (lanes) at a time. The list is walked once per block and each lane
// accumulates strictly in list order: no sum is ever reassociated, so each
// body's result is bit-identical across kernels and thread counts.
void Octree::evaluate(const Scratch& s, std::uint32_t first, std::uint32_t count, float eps2, float* outX,
                      float* outY, float* outZ) const noexcept {
  constexpr std::uint32_t kLanes = 8;
  LaneBlock blk;
  for (std::uint32_t base = 0; base < count; base += kLanes) {
    const std::uint32_t lanes = std::min(kLanes, count - base);
    for (std::uint32_t l = 0; l < kLanes; ++l) {
      const std::uint32_t k = first + base + (l < lanes ? l : lanes - 1);  // padding repeats a real body
      blk.px[l] = bx_[k];
      blk.py[l] = by_[k];
      blk.pz[l] = bz_[k];
    }
#if defined(FTV_BH_X86)
    if (use_avx_) {
      kernel_avx(blk, s.x.data(), s.y.data(), s.z.data(), s.q.data(), s.size, eps2);
    } else {
      kernel_sse2(blk, s.x.data(), s.y.data(), s.z.data(), s.q.data(), s.size, eps2);
    }
#else
    kernel_scalar(blk, s.x.data(), s.y.data(), s.z.data(), s.q.data(), s.size, eps2);
#endif
    for (std::uint32_t l = 0; l < lanes; ++l) {
      const std::uint32_t k = first + base + l;
      outX[k] = blk.ax[l];
      outY[k] = blk.ay[l];
      outZ[k] = blk.az[l];
    }
  }
}

std::uint64_t Octree::last_interactions() const noexcept {
  std::uint64_t total = 0;
  for (const Scratch& s : scratch_) total += s.interactions;
  return total;
}

void Octree::compute_repulsion(const RepulsionParams& params, float* outX, float* outY, float* outZ, ThreadPool& pool) {
  if (scratch_.size() < pool.concurrency()) scratch_.resize(pool.concurrency());
  for (Scratch& s : scratch_) s.interactions = 0;
  const float eps2 = params.softening * params.softening;

  // Groups are independent: each writes only its own contiguous slot range.
  // Chunks of 64 consecutive (Morton-ordered) groups keep each thread on a
  // compact region, so neighbouring groups reuse the cells their lists touch.
  pool.parallel_for(group_count_, 64, [&](std::size_t b, std::size_t e, unsigned slot) {
    Scratch& s = scratch_[slot];
    for (std::size_t g = b; g < e; ++g) {
      const std::uint32_t node = gnode_[g];
      const std::uint32_t f = gfirst_[g];
      const std::uint32_t c = gcount_[g];
      const std::uint32_t hub = nmeta_[node] & kMetaHubMask;
      if (hub != kMixedHub) {
        float mn[3] = {bx_[f], by_[f], bz_[f]};
        float mx[3] = {bx_[f], by_[f], bz_[f]};
        for (std::uint32_t k = f + 1; k < f + c; ++k) {
          mn[0] = std::min(mn[0], bx_[k]);
          mn[1] = std::min(mn[1], by_[k]);
          mn[2] = std::min(mn[2], bz_[k]);
          mx[0] = std::max(mx[0], bx_[k]);
          mx[1] = std::max(mx[1], by_[k]);
          mx[2] = std::max(mx[2], bz_[k]);
        }
        s.clear();
        traverse(mn, mx, static_cast<std::uint16_t>(hub), node, params, s);
        s.interactions += static_cast<std::uint64_t>(s.size) * c;
        evaluate(s, f, c, eps2, outX, outY, outZ);
      } else {
        // Mixed-hub leaf (rare: only where hub volumes touch): per-body lists,
        // because the same-hub / cross-hub scale depends on the body's hub.
        for (std::uint32_t k = f; k < f + c; ++k) {
          const float pt[3] = {bx_[k], by_[k], bz_[k]};
          s.clear();
          traverse(pt, pt, bhub_[k], node, params, s);
          s.interactions += s.size;
          evaluate(s, k, 1, eps2, outX, outY, outZ);
        }
      }
    }
  });
}

void brute_force_repulsion(const float* x, const float* y, const float* z, const float* q, const std::uint16_t* hub,
                           std::size_t n, const RepulsionParams& params, float* outX, float* outY, float* outZ) {
  const double eps2 = static_cast<double>(params.softening) * params.softening;
  for (std::size_t i = 0; i < n; ++i) {
    double ax = 0, ay = 0, az = 0;
    for (std::size_t j = 0; j < n; ++j) {
      if (i == j) continue;
      const double dx = static_cast<double>(x[i]) - x[j];
      const double dy = static_cast<double>(y[i]) - y[j];
      const double dz = static_cast<double>(z[i]) - z[j];
      const double r2 = dx * dx + dy * dy + dz * dz + eps2;
      const double factor = hub[i] == hub[j] ? 1.0 : params.cross_hub_scale;
      const double w = q[j] * factor / (r2 * std::sqrt(r2));
      ax += w * dx;
      ay += w * dy;
      az += w * dz;
    }
    outX[i] = static_cast<float>(ax);
    outY[i] = static_cast<float>(ay);
    outZ[i] = static_cast<float>(az);
  }
}

}  // namespace ftv
