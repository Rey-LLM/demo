// Layout determinism and structural sanity on a synthetic hub graph.

#include <cmath>
#include <string>

#include "layout/force_solver.hpp"
#include "layout/initial_placement.hpp"
#include "layout/layout_engine.hpp"
#include "model/hub_taxonomy.hpp"
#include "test_framework.hpp"

using namespace ftv;

namespace {

HubTaxonomy load_taxonomy() {
  std::string err;
  auto t = HubTaxonomy::load(std::string(FTVIZ_SOURCE_DIR) + "/config/hubs.json", err);
  if (!t) std::fprintf(stderr, "%s\n", err.c_str());
  return *t;
}

// Small deterministic graph: nodes spread over the first 5 hubs with ring
// edges inside each hub and a few inter-hub edges.
Graph make_graph(const HubTaxonomy& tax, std::size_t n) {
  Graph g;
  g.nodes.key.resize(n);
  g.nodes.hub.resize(n);
  g.nodes.flags.assign(n, 0);
  g.nodes.size.assign(n, 2.0f);
  g.nodes.weighted_degree.assign(n, 1.0f);
  g.nodes.degree.assign(n, 0);
  g.hub_population.assign(tax.size(), 0);
  for (std::size_t i = 0; i < n; ++i) {
    g.nodes.key[i] = 0x9E3779B97F4A7C15ull * (i + 1);
    g.nodes.hub[i] = static_cast<std::uint16_t>(i % 5);
    ++g.hub_population[g.nodes.hub[i]];
  }
  std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
  for (std::size_t i = 0; i + 5 < n; ++i) pairs.emplace_back(i, i + 5);         // intra-hub chains
  for (std::size_t i = 0; i + 1 < n; i += 50) pairs.emplace_back(i, i + 1);     // inter-hub
  g.edges.resize(pairs.size());
  for (std::size_t e = 0; e < pairs.size(); ++e) {
    g.edges.src[e] = pairs[e].first;
    g.edges.dst[e] = pairs[e].second;
    g.edges.weight[e] = 1000 * static_cast<std::int64_t>(e % 17 + 1);
    g.edges.count[e] = static_cast<std::uint32_t>(e % 5 + 1);
    g.edges.currency[e] = 0x445355;
    g.edges.channel[e] = 0;
    g.edges.flags[e] = g.nodes.hub[pairs[e].first] != g.nodes.hub[pairs[e].second] ? kEdgeInterHub : 0;
  }
  auto& adj = g.adjacency;
  adj.offset.assign(n + 1, 0);
  for (auto& p : pairs) {
    ++adj.offset[p.first + 1];
    ++adj.offset[p.second + 1];
  }
  for (std::size_t i = 0; i < n; ++i) adj.offset[i + 1] += adj.offset[i];
  adj.neighbor.resize(adj.offset[n]);
  adj.edge.resize(adj.offset[n]);
  std::vector<std::uint32_t> cur(adj.offset.begin(), adj.offset.end() - 1);
  for (std::size_t e = 0; e < pairs.size(); ++e) {
    auto [s, d] = pairs[e];
    adj.neighbor[cur[s]] = d;
    adj.edge[cur[s]++] = static_cast<std::uint32_t>(e);
    adj.neighbor[cur[d]] = s;
    adj.edge[cur[d]++] = static_cast<std::uint32_t>(e);
  }
  return g;
}

std::uint64_t run(const Graph& g, const HubTaxonomy& tax, unsigned workers, int iters) {
  ThreadPool pool(workers);
  LayoutParams p;
  ForceSolver solver(g, tax, p, pool);
  for (int i = 0; i < iters; ++i) solver.step();
  return solver.checksum();
}

}  // namespace

TEST_CASE("layout: initial placement lies inside the unit ball and is deterministic") {
  for (std::uint64_t k = 1; k < 5000; ++k) {
    const Float3 p = unit_ball_point(k * 0x9E3779B97F4A7C15ull);
    CHECK(p.x * p.x + p.y * p.y + p.z * p.z <= 1.0001f);
  }
  const Float3 a = unit_ball_point(12345), b = unit_ball_point(12345);
  CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
}

TEST_CASE("layout: identical checksum across runs and thread counts") {
  const HubTaxonomy tax = load_taxonomy();
  const Graph g = make_graph(tax, 4000);
  const std::uint64_t a = run(g, tax, 3, 40);
  const std::uint64_t b = run(g, tax, 3, 40);
  const std::uint64_t c = run(g, tax, 0, 40);  // single-threaded
  const std::uint64_t d = run(g, tax, 7, 40);
  std::fprintf(stderr, "  checksums: %016llx %016llx %016llx %016llx\n", static_cast<unsigned long long>(a),
               static_cast<unsigned long long>(b), static_cast<unsigned long long>(c),
               static_cast<unsigned long long>(d));
  CHECK_EQ(a, b);
  CHECK_EQ(a, c);
  CHECK_EQ(a, d);
}

TEST_CASE("layout: nodes stay clustered around their hub anchors and converge") {
  const HubTaxonomy tax = load_taxonomy();
  const Graph g = make_graph(tax, 3000);
  ThreadPool pool(3);
  LayoutParams p;
  ForceSolver solver(g, tax, p, pool);
  IterationStats st;
  for (int i = 0; i < 3000 && !solver.converged(); ++i) st = solver.step();
  std::fprintf(stderr, "  converged=%d after %llu iterations, mean disp %.4f\n", solver.converged() ? 1 : 0,
               static_cast<unsigned long long>(solver.iteration()), st.mean_displacement);
  CHECK(solver.converged());
  std::size_t outside = 0;
  for (std::size_t i = 0; i < g.node_count(); ++i) {
    const auto& h = tax.hubs()[g.nodes.hub[i]];
    const glm::vec3 pos = solver.position(i);
    const float dx = pos.x - h.anchor.x, dy = pos.y - h.anchor.y, dz = pos.z - h.anchor.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > 1.25f * h.radius) ++outside;
  }
  std::fprintf(stderr, "  nodes outside 1.25R: %zu / %zu\n", outside, g.node_count());
  CHECK(outside < g.node_count() / 100);
}

TEST_CASE("layout: engine publishes snapshots through the triple buffer") {
  const HubTaxonomy tax = load_taxonomy();
  const Graph g = make_graph(tax, 1000);
  ThreadPool pool(2);
  LayoutParams p;
  ForceSolver solver(g, tax, p, pool);
  LayoutEngine engine(solver, g.node_count());
  engine.publish_now();
  CHECK(engine.snapshots().acquire_latest());
  CHECK_EQ(engine.snapshots().front().xyz.size(), g.node_count() * 3);
  engine.start();
  std::uint64_t lastIter = 0;
  for (int spins = 0; spins < 2000 && lastIter < 5; ++spins) {
    if (engine.snapshots().acquire_latest()) lastIter = engine.snapshots().front().iteration;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(lastIter >= 5);
  engine.set_paused(true);
  engine.stop();
}

TEST_CASE("layout: internal (hub + BFS) node order maps back to graph order") {
  const HubTaxonomy tax = load_taxonomy();
  const Graph g = make_graph(tax, 1200);
  ThreadPool pool(2);
  LayoutParams p;
  ForceSolver solver(g, tax, p, pool);
  // Right after construction every node sits at its deterministic initial
  // placement, which depends only on its own key and hub.
  bool placed = true;
  for (std::size_t i = 0; i < g.node_count(); ++i) {
    const HubDef& h = tax.hubs()[g.nodes.hub[i]];
    const Float3 u = unit_ball_point(g.nodes.key[i]);
    const float r = h.radius * p.fill_fraction;
    const glm::vec3 pos = solver.position(i);
    placed &= pos.x == h.anchor.x + r * u.x && pos.y == h.anchor.y + r * u.y && pos.z == h.anchor.z + r * u.z;
  }
  CHECK(placed);
  for (int it = 0; it < 5; ++it) solver.step();
  std::vector<float> packed(g.node_count() * 3);
  solver.write_packed_xyz(packed.data());
  bool same = true;
  for (std::size_t i = 0; i < g.node_count(); ++i) {
    const glm::vec3 pos = solver.position(i);
    same &= packed[3 * i] == pos.x && packed[3 * i + 1] == pos.y && packed[3 * i + 2] == pos.z;
  }
  CHECK(same);
}
