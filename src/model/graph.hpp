#pragma once
// The structured transaction graph in Structure-of-Arrays form.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/aligned_allocator.hpp"
#include "model/edge_list.hpp"

namespace ftv {

enum NodeFlag : std::uint16_t {
  kNodeBridge = 1u << 0,      // seen in both fiat and on-chain flows (FLAG_BRIDGE)
  kNodeFiat = 1u << 1,        // has fiat-domain activity
  kNodeChain = 1u << 2,       // has chain-domain activity
  kNodeUnresolved = 1u << 3,  // assigned to the fallback hub
};

struct NodeArrays {
  AlignedVector<std::uint64_t> key;    // key64 (sorted ascending == dense order)
  AlignedVector<std::uint16_t> hub;    // hub_id
  AlignedVector<std::uint16_t> flags;  // NodeFlag bits
  AlignedVector<float> size;           // render/physics radius scalar
  AlignedVector<float> weighted_degree;  // sum of incident tx_count
  AlignedVector<std::uint32_t> degree;

  [[nodiscard]] std::size_t size_n() const noexcept { return key.size(); }
};

// Undirected incidence lists (CSR). Node i's incident edges are
// edge[offset[i] .. offset[i+1]) and the opposite endpoints are neighbor[...].
struct Adjacency {
  AlignedVector<std::uint32_t> offset;
  AlignedVector<std::uint32_t> neighbor;
  AlignedVector<std::uint32_t> edge;
};

struct Graph {
  NodeArrays nodes;
  EdgeList edges;
  Adjacency adjacency;
  std::vector<std::uint32_t> hub_population;  // nodes per hub
  std::vector<std::uint32_t> currencies;      // distinct packed currency codes, sorted (palette order)
  std::uint64_t bridge_count = 0;
  std::uint64_t inter_hub_edges = 0;
  std::uint64_t dropped_edges = 0;

  [[nodiscard]] std::size_t node_count() const noexcept { return nodes.key.size(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return edges.size(); }
  // Palette slot of a currency: 1 + rank among distinct currencies (0 = unknown).
  [[nodiscard]] std::uint32_t currency_palette_index(std::uint32_t code) const noexcept;
};

}  // namespace ftv
