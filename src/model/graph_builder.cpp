#include "model/graph_builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/log.hpp"
#include "ingest/types.hpp"

namespace ftv {

std::uint32_t Graph::currency_palette_index(std::uint32_t code) const noexcept {
  auto it = std::lower_bound(currencies.begin(), currencies.end(), code);
  if (it == currencies.end() || *it != code) return 0;
  return static_cast<std::uint32_t>(std::min<std::ptrdiff_t>(1 + (it - currencies.begin()), 127));
}

std::uint16_t assign_hub(const std::uint64_t* codes, const std::uint32_t* counts, std::size_t n,
                         const HubTaxonomy& taxonomy, std::uint16_t& flags) noexcept {
  // "Volume" for the majority vote is the endpoint transaction count: amounts
  // in different currencies are not comparable offline (no FX table), counts
  // are. Ties break toward the lower hub id. Location lists are tiny (usually
  // one entry), so a linear scan over a small fixed array is fastest.
  constexpr std::size_t kSlots = 16;
  std::array<std::uint16_t, kSlots> hubIds{};
  std::array<std::uint64_t, kSlots> hubVotes{};
  std::size_t used = 0;
  bool fiat = false, chain = false;
  const std::uint16_t fb = taxonomy.fallback();
  std::uint16_t best = fb;
  std::uint64_t bestVotes = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (codes[i] & kChainDomainBit) {
      chain = true;
    } else {
      fiat = true;
    }
    const std::uint16_t h = taxonomy.hub_for_location(codes[i]);
    if (h == fb) continue;
    std::size_t s = 0;
    while (s < used && hubIds[s] != h) ++s;
    if (s == used) {
      if (used == kSlots) {
        // Pathological entity active in >16 hubs: fold into the running best
        // without slot tracking (still deterministic: input order is sorted).
        if (counts[i] > bestVotes || (counts[i] == bestVotes && h < best)) {
          best = h;
          bestVotes = counts[i];
        }
        continue;
      }
      hubIds[used] = h;
      hubVotes[used] = 0;
      ++used;
    }
    hubVotes[s] += counts[i];
    if (hubVotes[s] > bestVotes || (hubVotes[s] == bestVotes && h < best)) {
      best = h;
      bestVotes = hubVotes[s];
    }
  }
  std::uint16_t f = 0;
  if (fiat) f |= kNodeFiat;
  if (chain) f |= kNodeChain;
  if (fiat && chain) f |= kNodeBridge;
  if (best == fb) f |= kNodeUnresolved;
  flags = f;
  return best;
}

Graph build_graph(const IngestResult& ingest, EdgeList&& edges, const HubTaxonomy& taxonomy, ThreadPool& pool,
                  const GraphBuildOptions& options) {
  Graph g;
  const std::size_t N = ingest.entity_count();
  g.nodes.key = ingest.keys;
  g.nodes.hub.assign(N, taxonomy.fallback());
  g.nodes.flags.assign(N, 0);
  g.nodes.size.assign(N, options.node_base_size);
  g.nodes.weighted_degree.assign(N, 0.0f);
  g.nodes.degree.assign(N, 0);

  // ---- hub assignment (parallel, disjoint writes) --------------------------
  const EntityLocations& loc = ingest.locations;
  if (loc.offset.size() == N + 1) {
    pool.parallel_for_range(N, 8192, [&](std::size_t b, std::size_t e) {
      for (std::size_t i = b; i < e; ++i) {
        const std::uint32_t o = loc.offset[i];
        const std::uint32_t c = loc.offset[i + 1] - o;
        std::uint16_t flags = 0;
        g.nodes.hub[i] = assign_hub(loc.code.data() + o, loc.count.data() + o, c, taxonomy, flags);
        g.nodes.flags[i] = flags;
      }
    });
  }
  g.hub_population.assign(taxonomy.size(), 0);
  for (std::size_t i = 0; i < N; ++i) {
    ++g.hub_population[g.nodes.hub[i]];
    g.bridge_count += (g.nodes.flags[i] & kNodeBridge) ? 1u : 0u;
  }

  // ---- validate edges (never trust indices blindly) ------------------------
  {
    std::size_t w = 0;
    const std::size_t E = edges.size();
    for (std::size_t i = 0; i < E; ++i) {
      if (edges.src[i] >= N || edges.dst[i] >= N) {
        ++g.dropped_edges;
        continue;
      }
      if (w != i) {
        edges.src[w] = edges.src[i];
        edges.dst[w] = edges.dst[i];
        edges.weight[w] = edges.weight[i];
        edges.count[w] = edges.count[i];
        edges.currency[w] = edges.currency[i];
        edges.channel[w] = edges.channel[i];
        edges.flags[w] = edges.flags[i];
        edges.first_ts[w] = edges.first_ts[i];
        edges.last_ts[w] = edges.last_ts[i];
      }
      ++w;
    }
    if (w != E) {
      FTV_LOG_WARN("dropped %zu edges with out-of-range endpoints", E - w);
      edges.resize(w);
    }
  }
  g.edges = std::move(edges);
  const std::size_t E = g.edges.size();

  // ---- classify edges ------------------------------------------------------
  pool.parallel_for_range(E, 1u << 16, [&](std::size_t b, std::size_t e) {
    for (std::size_t i = b; i < e; ++i) {
      std::uint8_t f = g.edges.flags[i] & static_cast<std::uint8_t>(~kEdgeInterHub);
      if (g.nodes.hub[g.edges.src[i]] != g.nodes.hub[g.edges.dst[i]]) f |= kEdgeInterHub;
      g.edges.flags[i] = f;
    }
  });
  for (std::size_t i = 0; i < E; ++i) g.inter_hub_edges += (g.edges.flags[i] & kEdgeInterHub) ? 1u : 0u;

  // ---- currency palette ----------------------------------------------------
  {
    std::vector<std::uint32_t> cur(g.edges.currency.begin(), g.edges.currency.end());
    std::sort(cur.begin(), cur.end());
    cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
    g.currencies = std::move(cur);
  }

  // ---- CSR adjacency (undirected incidences, edge-index order) -------------
  auto& adj = g.adjacency;
  adj.offset.assign(N + 1, 0);
  for (std::size_t i = 0; i < E; ++i) {
    if (g.edges.flags[i] & kEdgeSelfLoop) continue;
    ++adj.offset[g.edges.src[i] + 1];
    ++adj.offset[g.edges.dst[i] + 1];
  }
  for (std::size_t i = 0; i < N; ++i) adj.offset[i + 1] += adj.offset[i];
  adj.neighbor.resize(adj.offset[N]);
  adj.edge.resize(adj.offset[N]);
  {
    AlignedVector<std::uint32_t> cursor(adj.offset.begin(), adj.offset.end() - 1);
    for (std::size_t i = 0; i < E; ++i) {
      if (g.edges.flags[i] & kEdgeSelfLoop) continue;
      const std::uint32_t s = g.edges.src[i], d = g.edges.dst[i];
      adj.neighbor[cursor[s]] = d;
      adj.edge[cursor[s]++] = static_cast<std::uint32_t>(i);
      adj.neighbor[cursor[d]] = s;
      adj.edge[cursor[d]++] = static_cast<std::uint32_t>(i);
    }
  }

  // ---- degree-weighted volume and node size --------------------------------
  pool.parallel_for_range(N, 8192, [&](std::size_t b, std::size_t e) {
    for (std::size_t i = b; i < e; ++i) {
      double wsum = 0.0;
      for (std::uint32_t k = adj.offset[i]; k < adj.offset[i + 1]; ++k) wsum += g.edges.count[adj.edge[k]];
      g.nodes.degree[i] = adj.offset[i + 1] - adj.offset[i];
      g.nodes.weighted_degree[i] = static_cast<float>(wsum);
      const float s = options.node_base_size * (1.0f + options.node_size_log_scale * static_cast<float>(std::log1p(wsum)));
      g.nodes.size[i] = std::min(s, options.node_max_size);
    }
  });
  return g;
}

}  // namespace ftv
