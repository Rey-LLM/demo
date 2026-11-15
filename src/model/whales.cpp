#include "model/whales.hpp"

#include <algorithm>
#include <limits>

namespace ftv {

std::uint32_t whale_tier(std::uint32_t rank) noexcept {
  if (rank < 2) return 3;
  if (rank < 6) return 2;
  return 1;
}

WhaleSet build_whales(const IngestResult& ingest, const Graph& graph, float replaySeconds, float tailSeconds) {
  WhaleSet out;
  out.replay_seconds = replaySeconds;
  const EdgeList& e = graph.edges;
  std::int64_t t0 = std::numeric_limits<std::int64_t>::max(), t1 = std::numeric_limits<std::int64_t>::min();
  for (std::size_t i = 0; i < e.size(); ++i) {
    t0 = std::min(t0, e.first_ts[i]);
    t1 = std::max(t1, e.last_ts[i]);
  }
  if (e.size() == 0) t0 = t1 = 0;
  out.t0_ms = t0;
  out.t1_ms = t1;

  const auto& keys = graph.nodes.key;  // sorted ascending == dense order
  auto find = [&keys](std::uint64_t k, std::uint32_t& idx) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), k);
    if (it == keys.end() || *it != k) return false;
    idx = static_cast<std::uint32_t>(it - keys.begin());
    return true;
  };
  const double span = std::max<double>(1.0, static_cast<double>(t1 - t0));
  const double usable = std::max(0.0, static_cast<double>(replaySeconds - tailSeconds));
  std::uint32_t rank = 0;
  std::uint32_t lastCurrency = 0;
  for (std::size_t i = 0; i < ingest.whales.size(); ++i) {
    const WhaleCandidate& c = ingest.whales[i];
    rank = (i > 0 && c.currency == lastCurrency) ? rank + 1 : 0;
    lastCurrency = c.currency;
    WhaleEvent w;
    if (!find(c.src, w.src) || !find(c.dst, w.dst) || w.src == w.dst) continue;
    w.currency = c.currency;
    w.rank = rank;
    w.tier = whale_tier(rank);
    w.amount_minor = c.amount_minor;
    w.ts_ms = c.ts_ms;
    w.start = static_cast<float>(static_cast<double>(c.ts_ms - t0) / span * usable);
    out.events.push_back(w);
  }
  std::sort(out.events.begin(), out.events.end(), [](const WhaleEvent& a, const WhaleEvent& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.src != b.src) return a.src < b.src;
    return a.dst < b.dst;
  });
  return out;
}

}  // namespace ftv
