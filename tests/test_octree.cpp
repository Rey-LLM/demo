// Barnes-Hut accuracy against the O(N^2) reference.

#include <cmath>
#include <random>
#include <vector>

#include "layout/octree.hpp"
#include "test_framework.hpp"

using namespace ftv;

namespace {

struct Cloud {
  std::vector<float> x, y, z, q;
  std::vector<std::uint16_t> hub;
};

// Clustered cloud resembling the hub layout: several Gaussian blobs with
// sub-clusters, two hubs sharing a region to exercise mixed cells.
Cloud make_cloud(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> g(0.0f, 1.0f);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  const float centers[5][3] = {{0, 0, 0}, {1000, 0, 0}, {0, 0, 1000}, {500, 600, 500}, {80, 30, 0}};
  Cloud c;
  for (std::size_t i = 0; i < n; ++i) {
    const auto h = static_cast<std::uint16_t>(i % 5);
    const float spread = h == 4 ? 60.0f : 150.0f;
    const float sub = u(rng) < 0.5f ? 0.3f : 1.0f;
    c.x.push_back(centers[h][0] + g(rng) * spread * sub);
    c.y.push_back(centers[h][1] + g(rng) * spread * sub);
    c.z.push_back(centers[h][2] + g(rng) * spread * sub);
    c.q.push_back(0.5f + u(rng));
    c.hub.push_back(h);
  }
  return c;
}

double relative_error(const Cloud& c, const RepulsionParams& p, unsigned leaf, ThreadPool& pool) {
  const std::size_t n = c.x.size();
  Octree tree;
  tree.build(c.x.data(), c.y.data(), c.z.data(), c.q.data(), c.hub.data(), n, leaf, pool);
  std::vector<float> sx(n), sy(n), sz(n);
  tree.compute_repulsion(p, sx.data(), sy.data(), sz.data(), pool);
  std::vector<float> bx(n), by(n), bz(n);
  brute_force_repulsion(c.x.data(), c.y.data(), c.z.data(), c.q.data(), c.hub.data(), n, p, bx.data(), by.data(),
                        bz.data());
  // Global relative error: ||F_bh - F_exact|| / ||F_exact|| over all bodies.
  double num = 0, den = 0;
  const auto order = tree.order();
  for (std::size_t k = 0; k < n; ++k) {
    const std::uint32_t i = order[k];
    const double dx = sx[k] - bx[i], dy = sy[k] - by[i], dz = sz[k] - bz[i];
    num += dx * dx + dy * dy + dz * dz;
    den += static_cast<double>(bx[i]) * bx[i] + static_cast<double>(by[i]) * by[i] + static_cast<double>(bz[i]) * bz[i];
  }
  return std::sqrt(num / den);
}

}  // namespace

TEST_CASE("octree: Barnes-Hut force within 2% of brute force at theta=0.8") {
  ThreadPool pool(3);
  const Cloud c = make_cloud(6000, 7);
  RepulsionParams p;
  p.theta = 0.8f;
  p.softening = 1.0f;
  p.cross_hub_scale = 0.05f;
  const double err = relative_error(c, p, 16, pool);
  std::fprintf(stderr, "  BH relative error (theta=0.8, leaf=16): %.4f%%\n", err * 100.0);
  CHECK(err < 0.02);
  // Small leaves exercise deeper trees and more approximations.
  const double err4 = relative_error(c, p, 4, pool);
  std::fprintf(stderr, "  BH relative error (theta=0.8, leaf=4):  %.4f%%\n", err4 * 100.0);
  CHECK(err4 < 0.02);
}

TEST_CASE("octree: theta=0 is exact (all cells opened)") {
  ThreadPool pool(2);
  const Cloud c = make_cloud(800, 11);
  RepulsionParams p;
  p.theta = 0.0f;
  CHECK(relative_error(c, p, 8, pool) < 1e-4);
}

TEST_CASE("octree: structure invariants") {
  ThreadPool pool(2);
  const Cloud c = make_cloud(5000, 3);
  Octree tree;
  tree.build(c.x.data(), c.y.data(), c.z.data(), c.q.data(), c.hub.data(), c.x.size(), 16, pool);
  CHECK(tree.node_count() > tree.leaf_count());
  CHECK(tree.leaf_count() >= c.x.size() / 16);
  // order() is a permutation.
  std::vector<int> seen(c.x.size(), 0);
  for (auto i : tree.order()) ++seen[i];
  bool perm = true;
  for (int s : seen) perm &= s == 1;
  CHECK(perm);
  // Tiny inputs: single leaf / empty.
  Octree small;
  small.build(c.x.data(), c.y.data(), c.z.data(), c.q.data(), c.hub.data(), 5, 16, pool);
  CHECK_EQ(small.node_count(), 1u);
  Octree empty;
  empty.build(c.x.data(), c.y.data(), c.z.data(), c.q.data(), c.hub.data(), 0, 16, pool);
  CHECK_EQ(empty.node_count(), 0u);
}
