#pragma once
// Inter-hub corridor bundles: all inter-hub edges sharing (hub_a, hub_b,
// currency) collapse into one bundle drawn as a Bezier ribbon between the
// source hub's exit port and the target hub's entry port.

#include <cstdint>
#include <vector>

#include "model/graph.hpp"

namespace ftv {

struct CorridorBundle {
  std::uint16_t hub_a = 0;  // source hub
  std::uint16_t hub_b = 0;  // target hub
  std::uint32_t currency = 0;
  std::uint32_t palette = 0;      // palette slot of the currency
  std::int64_t volume = 0;        // sum |total_amount| (minor units, clamped)
  std::uint64_t tx_count = 0;
  std::uint32_t edge_count = 0;
  float thickness = 1.0f;   // [0.2, 1]: volume relative to the largest bundle of the same currency
  float brightness = 1.0f;  // [0.25, 1]: log tx_count relative to the busiest bundle
};

std::vector<CorridorBundle> build_corridor_bundles(const Graph& graph);

}  // namespace ftv
