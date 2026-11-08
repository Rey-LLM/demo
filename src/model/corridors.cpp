#include "model/corridors.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "ingest/int128_acc.hpp"

namespace ftv {

std::vector<CorridorBundle> build_corridor_bundles(const Graph& g) {
  struct Acc {
    Int128Acc volume;
    std::uint64_t tx = 0;
    std::uint32_t edges = 0;
  };
  // Startup-only: an ordered map keeps the bundle order deterministic.
  std::map<std::tuple<std::uint16_t, std::uint16_t, std::uint32_t>, Acc> acc;
  const std::size_t E = g.edge_count();
  for (std::size_t i = 0; i < E; ++i) {
    if (!(g.edges.flags[i] & kEdgeInterHub)) continue;
    const auto key = std::make_tuple(g.nodes.hub[g.edges.src[i]], g.nodes.hub[g.edges.dst[i]], g.edges.currency[i]);
    Acc& a = acc[key];
    const std::int64_t w = g.edges.weight[i];
    a.volume.add(w == INT64_MIN ? INT64_MAX : (w < 0 ? -w : w));
    a.tx += g.edges.count[i];
    ++a.edges;
  }
  std::vector<CorridorBundle> out;
  out.reserve(acc.size());
  std::map<std::uint32_t, std::int64_t> maxVolPerCurrency;
  std::uint64_t maxTx = 1;
  for (const auto& [k, a] : acc) {
    CorridorBundle b;
    b.hub_a = std::get<0>(k);
    b.hub_b = std::get<1>(k);
    b.currency = std::get<2>(k);
    b.palette = g.currency_palette_index(b.currency);
    b.volume = a.volume.clamp_to_int64();
    b.tx_count = a.tx;
    b.edge_count = a.edges;
    auto& mv = maxVolPerCurrency[b.currency];
    mv = std::max(mv, b.volume);
    maxTx = std::max(maxTx, b.tx_count);
    out.push_back(b);
  }
  const double logMaxTx = std::log1p(static_cast<double>(maxTx));
  for (auto& b : out) {
    // Volumes are compared within a currency only (no FX conversion offline).
    const double mv = static_cast<double>(std::max<std::int64_t>(1, maxVolPerCurrency[b.currency]));
    const double rel = static_cast<double>(b.volume) / mv;
    b.thickness = static_cast<float>(0.2 + 0.8 * std::sqrt(std::clamp(rel, 0.0, 1.0)));
    b.brightness = static_cast<float>(0.25 + 0.75 * std::log1p(static_cast<double>(b.tx_count)) / logMaxTx);
  }
  return out;
}

}  // namespace ftv
