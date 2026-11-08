#pragma once
// Builds the Graph from ingestion output: deterministic hub assignment
// (majority hub by transaction count, BRIDGE detection), edge classification,
// index validation, CSR adjacency and node sizes.

#include "core/thread_pool.hpp"
#include "ingest/ingest.hpp"
#include "model/graph.hpp"
#include "model/hub_taxonomy.hpp"

namespace ftv {

struct GraphBuildOptions {
  float node_base_size = 1.6f;
  float node_size_log_scale = 0.45f;
  float node_max_size = 9.0f;
};

Graph build_graph(const IngestResult& ingest, EdgeList&& edges, const HubTaxonomy& taxonomy, ThreadPool& pool,
                  const GraphBuildOptions& options = {});

// Hub assignment for one entity from its location votes (exposed for tests).
// Returns the hub id and writes NodeFlag bits.
std::uint16_t assign_hub(const std::uint64_t* codes, const std::uint32_t* counts, std::size_t n,
                         const HubTaxonomy& taxonomy, std::uint16_t& flags) noexcept;

}  // namespace ftv
