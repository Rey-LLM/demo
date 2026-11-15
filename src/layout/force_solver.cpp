#include "layout/force_solver.hpp"

#include <xxhash.h>

#include <algorithm>
#include <cmath>

#include "core/timing.hpp"
#include "layout/initial_placement.hpp"

namespace ftv {

namespace {
constexpr std::size_t kIntegrateGrain = 4096;

constexpr float smoothstep(float e0, float e1, float x) noexcept {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
}  // namespace

ForceSolver::ForceSolver(const Graph& graph, const HubTaxonomy& taxonomy, const LayoutParams& params, ThreadPool& pool)
    : pool_(pool), params_(params), n_(graph.node_count()) {
  const auto alloc = [this](AlignedVector<float>& v) { v.assign(n_, 0.0f); };
  for (auto* v : {&x_, &y_, &z_, &nx_, &ny_, &nz_, &vx_, &vy_, &vz_, &pax_, &pay_, &paz_, &charge_, &rep_x_,
                  &rep_y_, &rep_z_}) {
    alloc(*v);
  }
  build_order(graph);
  hub_.resize(n_);
  key_.resize(n_);
  for (std::size_t a = 0; a < n_; ++a) {
    hub_[a] = graph.nodes.hub[perm_[a]];
    key_[a] = graph.nodes.key[perm_[a]];
  }
  inverse_.assign(n_, 0);
  chunk_disp_.assign(ThreadPool::chunk_count(n_, kIntegrateGrain) + 1, 0.0);

  const auto& hubs = taxonomy.hubs();
  const std::size_t H = hubs.size();
  hub_ax_.resize(H);
  hub_ay_.resize(H);
  hub_az_.resize(H);
  hub_r_.resize(H);
  hub_k_.resize(H);
  hub_core_.resize(H);
  for (std::size_t h = 0; h < H; ++h) {
    hub_ax_[h] = hubs[h].anchor.x;
    hub_ay_[h] = hubs[h].anchor.y;
    hub_az_[h] = hubs[h].anchor.z;
    hub_r_[h] = hubs[h].radius;
    hub_k_[h] = hubs[h].k_anchor;
    hub_core_[h] = params_.core_gravity_ratio * hubs[h].k_anchor;
  }

  // Population-normalised charge. A uniform ball of total charge Q and radius
  // r produces an interior field Q*s/r^3, which exactly cancels a harmonic
  // pull k_core*s when Q = k_core * r^3. Choosing r = fill * R per hub makes
  // every hub fill the same fraction of its radius whatever its population.
  for (std::size_t i = 0; i < n_; ++i) {
    const std::uint16_t h = hub_[i];
    const double r = static_cast<double>(params_.fill_fraction) * hub_r_[h];
    const double pop = std::max<std::uint32_t>(1, graph.hub_population[h]);
    charge_[i] = static_cast<float>(static_cast<double>(hub_core_[h]) * r * r * r / pop);
  }
  build_springs(graph);
  reset();
}

void ForceSolver::build_order(const Graph& graph) {
  perm_.resize(n_);
  rank_.resize(n_);
  if (n_ == 0) return;
  const auto& hub = graph.nodes.hub;
  const auto& adj = graph.adjacency;
  std::size_t hubCount = 0;
  for (std::size_t i = 0; i < n_; ++i) hubCount = std::max<std::size_t>(hubCount, hub[i] + 1u);
  // Bucket nodes by hub, keeping graph order inside each bucket (BFS seeds).
  std::vector<std::uint32_t> start(hubCount + 1, 0);
  for (std::size_t i = 0; i < n_; ++i) ++start[hub[i] + 1u];
  for (std::size_t h = 0; h < hubCount; ++h) start[h + 1] += start[h];
  std::vector<std::uint32_t> byHub(n_);
  {
    std::vector<std::uint32_t> pos(start.begin(), start.end() - 1);
    for (std::size_t i = 0; i < n_; ++i) byHub[pos[hub[i]]++] = static_cast<std::uint32_t>(i);
  }
  // Breadth-first per hub; perm_ doubles as the BFS queue. Deterministic:
  // seeds in graph order, neighbours in CSR order.
  std::vector<std::uint8_t> seen(n_, 0);
  std::size_t out = 0;
  for (std::size_t h = 0; h < hubCount; ++h) {
    for (std::uint32_t si = start[h]; si < start[h + 1]; ++si) {
      const std::uint32_t seed = byHub[si];
      if (seen[seed]) continue;
      seen[seed] = 1;
      std::size_t head = out;
      perm_[out++] = seed;
      while (head < out) {
        const std::uint32_t v = perm_[head++];
        for (std::uint32_t k = adj.offset[v]; k < adj.offset[v + 1]; ++k) {
          const std::uint32_t u = adj.neighbor[k];
          if (!seen[u] && hub[u] == h) {
            seen[u] = 1;
            perm_[out++] = u;
          }
        }
      }
    }
  }
  for (std::size_t a = 0; a < n_; ++a) rank_[perm_[a]] = static_cast<std::uint32_t>(a);
}

void ForceSolver::build_springs(const Graph& graph) {
  const auto& adj = graph.adjacency;
  const auto& e = graph.edges;
  // CSR incidences re-indexed into internal order (same per-node order).
  spring_offset_.resize(n_ + 1);
  spring_offset_[0] = 0;
  for (std::size_t a = 0; a < n_; ++a) {
    const std::uint32_t g = perm_[a];
    spring_offset_[a + 1] = spring_offset_[a] + (adj.offset[g + 1] - adj.offset[g]);
  }
  spring_other_.resize(adj.edge.size());
  std::vector<std::uint32_t> springEdge(adj.edge.size());
  for (std::size_t a = 0; a < n_; ++a) {
    const std::uint32_t g = perm_[a];
    std::uint32_t s = spring_offset_[a];
    for (std::uint32_t k = adj.offset[g]; k < adj.offset[g + 1]; ++k, ++s) {
      spring_other_[s] = rank_[adj.neighbor[k]];
      springEdge[s] = adj.edge[k];
    }
  }
  spring_rest_.resize(adj.edge.size());
  spring_k_.resize(adj.edge.size());
  double maxLog = 1e-9;
  for (std::size_t i = 0; i < e.size(); ++i) {
    const double w = static_cast<double>(e.weight[i]);
    maxLog = std::max(maxLog, std::log10(1.0 + std::fabs(w)));
  }
  for (std::size_t k = 0; k < springEdge.size(); ++k) {
    const std::uint32_t ei = springEdge[k];
    const bool inter = (e.flags[ei] & kEdgeInterHub) != 0;
    // Strength ~ log10(1 + |total_amount|), normalised to [0, 1].
    const double strength = std::log10(1.0 + std::fabs(static_cast<double>(e.weight[ei]))) / maxLog;
    float kk = params_.spring_k * static_cast<float>(strength);
    float rest;
    if (inter) {
      // Inter-hub springs rest at the hub separation and are attenuated, so
      // they orient bridge nodes toward partner hubs without pulling the
      // clusters together.
      const std::uint16_t ha = graph.nodes.hub[e.src[ei]], hb = graph.nodes.hub[e.dst[ei]];
      const float dx = hub_ax_[ha] - hub_ax_[hb], dy = hub_ay_[ha] - hub_ay_[hb], dz = hub_az_[ha] - hub_az_[hb];
      rest = std::sqrt(dx * dx + dy * dy + dz * dz);
      kk *= params_.inter_hub_spring_scale;
    } else {
      // L0 ~ 1 / ln(1 + tx_count): frequent counterparties sit closer.
      rest = params_.spring_rest_base / static_cast<float>(std::log1p(static_cast<double>(e.count[ei])));
      rest = std::clamp(rest, params_.spring_rest_min, params_.spring_rest_max);
    }
    spring_rest_[k] = rest;
    spring_k_[k] = kk;
  }
}

void ForceSolver::reset() {
  for (std::size_t i = 0; i < n_; ++i) {
    const std::uint16_t h = hub_[i];
    const Float3 u = unit_ball_point(key_[i]);
    const float r = hub_r_[h] * params_.fill_fraction;
    x_[i] = hub_ax_[h] + r * u.x;
    y_[i] = hub_ay_[h] + r * u.y;
    z_[i] = hub_az_[h] + r * u.z;
  }
  std::fill(vx_.begin(), vx_.end(), 0.0f);
  std::fill(vy_.begin(), vy_.end(), 0.0f);
  std::fill(vz_.begin(), vz_.end(), 0.0f);
  std::fill(pax_.begin(), pax_.end(), 0.0f);
  std::fill(pay_.begin(), pay_.end(), 0.0f);
  std::fill(paz_.begin(), paz_.end(), 0.0f);
  have_prev_accel_ = false;
  iteration_ = 0;
  temperature_ = params_.initial_temperature;
  calm_iterations_ = 0;
  converged_ = false;
}

void ForceSolver::reheat(float temperature) {
  temperature_ = std::max(temperature, params_.min_temperature);
  calm_iterations_ = 0;
  converged_ = false;
}

IterationStats ForceSolver::step() {
  IterationStats st;
  Stopwatch total;
  Stopwatch phase;
  if (n_ == 0) {
    converged_ = true;
    st.converged = true;
    return st;
  }

  tree_.build(x_.data(), y_.data(), z_.data(), charge_.data(), hub_.data(), n_, params_.leaf_size, pool_);
  st.tree_ms = phase.lap_ms();

  const RepulsionParams rp{params_.theta, params_.softening, params_.cross_hub_repulsion};
  tree_.compute_repulsion(rp, rep_x_.data(), rep_y_.data(), rep_z_.data(), pool_);
  st.repulsion_ms = phase.lap_ms();

  // Inverse permutation. Thread t owns bodies [lo, hi): it scans the whole
  // (cache-resident) order array and writes only its own range, so no two
  // threads ever write the same cache line.
  {
    const std::span<const std::uint32_t> order = tree_.order();
    const std::size_t parts = pool_.concurrency();
    const std::size_t n = n_;
    pool_.parallel_for(parts, 1, [&](std::size_t b, std::size_t e, unsigned) {
      for (std::size_t p = b; p < e; ++p) {
        const std::uint32_t lo = static_cast<std::uint32_t>(p * n / parts);
        const std::uint32_t hi = static_cast<std::uint32_t>((p + 1) * n / parts);
        for (std::size_t k = 0; k < n; ++k) {
          const std::uint32_t i = order[k];
          if (i - lo < hi - lo) inverse_[i] = static_cast<std::uint32_t>(k);  // unsigned range test
        }
      }
    });
  }

  const float dt = params_.dt;
  const float damping = params_.damping;
  const float T = temperature_;
  const float inner = params_.anchor_inner, outer = params_.anchor_outer;
  const bool havePrev = have_prev_accel_;
  pool_.parallel_for(n_, kIntegrateGrain, [&](std::size_t b, std::size_t e, unsigned) {
    double dispSum = 0.0;
    for (std::size_t i = b; i < e; ++i) {
      const std::uint32_t k = inverse_[i];
      float ax = rep_x_[k], ay = rep_y_[k], az = rep_z_[k];
      const float px = x_[i], py = y_[i], pz = z_[i];

      // Hub anchoring: F = -k_anchor * (p - a) * smoothstep(0.6R, 1.2R, |p - a|)
      // plus the weak harmonic core gravity that balances repulsion.
      const std::uint16_t h = hub_[i];
      const float dx = px - hub_ax_[h], dy = py - hub_ay_[h], dz = pz - hub_az_[h];
      const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float R = hub_r_[h];
      const float pull = hub_k_[h] * smoothstep(inner * R, outer * R, dist) + hub_core_[h];
      ax -= pull * dx;
      ay -= pull * dy;
      az -= pull * dz;

      // Hooke springs along incident aggregated edges (reads current
      // positions only; writes go to the next-position buffer).
      for (std::uint32_t s = spring_offset_[i]; s < spring_offset_[i + 1]; ++s) {
        const std::uint32_t j = spring_other_[s];
        const float sx = x_[j] - px, sy = y_[j] - py, sz = z_[j] - pz;
        const float d = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (d > 1e-4f) {
          const float f = spring_k_[s] * (d - spring_rest_[s]) / d;
          ax += f * sx;
          ay += f * sy;
          az += f * sz;
        }
      }

      // Velocity Verlet with damping: complete v(n) from a(n-1), a(n), then
      // drift x(n+1) = x(n) + v dt + a dt^2 / 2, clamped by temperature.
      const float pax = havePrev ? pax_[i] : ax;
      const float pay = havePrev ? pay_[i] : ay;
      const float paz = havePrev ? paz_[i] : az;
      float vx = (vx_[i] + 0.5f * (pax + ax) * dt) * damping;
      float vy = (vy_[i] + 0.5f * (pay + ay) * dt) * damping;
      float vz = (vz_[i] + 0.5f * (paz + az) * dt) * damping;
      float mx = vx * dt + 0.5f * ax * dt * dt;
      float my = vy * dt + 0.5f * ay * dt * dt;
      float mz = vz * dt + 0.5f * az * dt * dt;
      float len = std::sqrt(mx * mx + my * my + mz * mz);
      if (len > T) {
        const float sc = T / len;
        mx *= sc;
        my *= sc;
        mz *= sc;
        vx *= sc;
        vy *= sc;
        vz *= sc;
        len = T;
      }
      nx_[i] = px + mx;
      ny_[i] = py + my;
      nz_[i] = pz + mz;
      vx_[i] = vx;
      vy_[i] = vy;
      vz_[i] = vz;
      pax_[i] = ax;
      pay_[i] = ay;
      paz_[i] = az;
      dispSum += len;
    }
    chunk_disp_[b / kIntegrateGrain] = dispSum;  // one slot per fixed chunk
  });
  st.integrate_ms = phase.lap_ms();

  x_.swap(nx_);
  y_.swap(ny_);
  z_.swap(nz_);
  have_prev_accel_ = true;

  double disp = 0.0;
  const std::size_t chunks = ThreadPool::chunk_count(n_, kIntegrateGrain);
  for (std::size_t c = 0; c < chunks; ++c) disp += chunk_disp_[c];  // fixed order => deterministic
  st.mean_displacement = disp / static_cast<double>(n_);

  temperature_ = std::max(params_.min_temperature, temperature_ * params_.cooling);
  if (st.mean_displacement < params_.convergence_epsilon) {
    if (++calm_iterations_ >= params_.convergence_window) converged_ = true;
  } else {
    calm_iterations_ = 0;
  }
  ++iteration_;
  st.iteration = iteration_;
  st.temperature = temperature_;
  st.converged = converged_;
  st.total_ms = total.millis();
  return st;
}

std::uint64_t ForceSolver::checksum() const noexcept {
  std::uint64_t h = XXH3_64bits(x_.data(), n_ * sizeof(float));
  h = XXH3_64bits_withSeed(y_.data(), n_ * sizeof(float), h);
  h = XXH3_64bits_withSeed(z_.data(), n_ * sizeof(float), h);
  return h;
}

void ForceSolver::write_packed_xyz(float* dst) const {
  // Gather in graph order: sequential writes, each thread owns its range.
  pool_.parallel_for(n_, 32768, [&](std::size_t b, std::size_t e, unsigned) {
    for (std::size_t i = b; i < e; ++i) {
      const std::uint32_t a = rank_[i];
      dst[3 * i + 0] = x_[a];
      dst[3 * i + 1] = y_[a];
      dst[3 * i + 2] = z_[a];
    }
  });
}

}  // namespace ftv
