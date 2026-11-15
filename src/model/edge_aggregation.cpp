#include "model/edge_aggregation.hpp"

#include <algorithm>
#include <vector>

#include "core/timing.hpp"

namespace ftv {

namespace {

struct PartialEdge {
  std::uint32_t src;
  std::uint32_t dst;
  std::uint32_t currency;
  std::uint32_t channel;
  Int128Acc total;
  std::int64_t first_ts;
  std::int64_t last_ts;
  std::uint32_t count;
};

bool tuple_less(const PartialEdge& a, const PartialEdge& b) noexcept {
  if (a.src != b.src) return a.src < b.src;
  if (a.dst != b.dst) return a.dst < b.dst;
  if (a.currency != b.currency) return a.currency < b.currency;
  return a.channel < b.channel;
}
bool tuple_equal(const PartialEdge& a, const PartialEdge& b) noexcept {
  return a.src == b.src && a.dst == b.dst && a.currency == b.currency && a.channel == b.channel;
}

}  // namespace

EdgeList aggregate_edges(IngestResult& ingest, ThreadPool& pool) {
  Stopwatch phase;
  EdgeList out;
  const std::size_t N = ingest.entity_count();
  const std::size_t W = ingest.workers.size();
  if (N == 0 || W == 0) return out;
  const std::size_t P = std::max<std::size_t>(1, std::min<std::size_t>(1024, N));
  const EntityRegistry& reg = *ingest.registry;

  std::vector<std::vector<std::vector<PartialEdge>>> buckets(W, std::vector<std::vector<PartialEdge>>(P));
  std::vector<std::uint64_t> dropped(W, 0);
  pool.parallel_for_range(W, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t wi = b; wi < e; ++wi) {
      EdgeTable& table = ingest.workers[wi]->edges();
      auto& my = buckets[wi];
      for (auto& v : my) v.reserve(table.size() / P + 8);
      table.for_each([&](const EdgeKey& k, const EdgeVal& v) {
        const std::uint32_t s = reg.dense_of(k.src);
        const std::uint32_t d = reg.dense_of(k.dst);
        if (s >= N || d >= N) {  // cannot happen for a consistent registry; never trust it blindly
          ++dropped[wi];
          return;
        }
        const std::size_t p = static_cast<std::size_t>(static_cast<std::uint64_t>(s) * P / N);
        my[p].push_back(PartialEdge{s, d, k.currency, k.channel, v.total, v.first_ts, v.last_ts, v.count});
      });
      table.release();
    }
  });
  for (auto d : dropped) ingest.stats.dropped_invalid_edges += d;
  ingest.stats.phases.push_back({"edge_scatter", phase.lap_ms()});

  std::vector<std::vector<PartialEdge>> merged(P);
  pool.parallel_for_range(P, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t p = b; p < e; ++p) {
      std::size_t n = 0;
      for (std::size_t w = 0; w < W; ++w) n += buckets[w][p].size();
      auto& m = merged[p];
      m.reserve(n);
      for (std::size_t w = 0; w < W; ++w) {
        m.insert(m.end(), buckets[w][p].begin(), buckets[w][p].end());
        std::vector<PartialEdge>().swap(buckets[w][p]);
      }
      std::sort(m.begin(), m.end(), tuple_less);
      std::size_t o = 0;
      for (std::size_t i = 0; i < m.size(); ++i) {
        if (o > 0 && tuple_equal(m[o - 1], m[i])) {
          PartialEdge& t = m[o - 1];
          t.total.add(m[i].total);
          t.count += m[i].count;
          t.first_ts = std::min(t.first_ts, m[i].first_ts);
          t.last_ts = std::max(t.last_ts, m[i].last_ts);
        } else {
          m[o++] = m[i];
        }
      }
      m.resize(o);
    }
  });
  ingest.stats.phases.push_back({"edge_merge", phase.lap_ms()});

  std::vector<std::size_t> base(P + 1, 0);
  for (std::size_t p = 0; p < P; ++p) base[p + 1] = base[p] + merged[p].size();
  out.resize(base[P]);
  pool.parallel_for_range(P, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t p = b; p < e; ++p) {
      const auto& m = merged[p];
      for (std::size_t i = 0; i < m.size(); ++i) {
        const std::size_t j = base[p] + i;
        out.src[j] = m[i].src;
        out.dst[j] = m[i].dst;
        out.weight[j] = m[i].total.clamp_to_int64();
        out.count[j] = m[i].count;
        out.currency[j] = m[i].currency;
        out.channel[j] = static_cast<std::uint8_t>(m[i].channel);
        out.flags[j] = m[i].src == m[i].dst ? kEdgeSelfLoop : 0;
        out.first_ts[j] = m[i].first_ts;
        out.last_ts[j] = m[i].last_ts;
      }
      std::vector<PartialEdge>().swap(merged[p]);
    }
  });
  ingest.stats.aggregated_edges = out.size();
  ingest.stats.phases.push_back({"edge_soa", phase.lap_ms()});
  ingest.workers.clear();
  return out;
}

}  // namespace ftv
