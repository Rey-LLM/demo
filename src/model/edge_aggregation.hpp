#pragma once
// Parallel merge of the per-worker partial edge aggregations.
//
//  A. (parallel over workers) translate key64 endpoints to dense indices and
//     scatter partials into P partitions by src index range;
//  B. (parallel over partitions) sort each partition by the tuple and fold
//     duplicates (exact 128-bit amount sums, counts, min/max timestamps);
//  C. (parallel over partitions) write the SoA EdgeList at prefix offsets.
// The partition of an edge depends only on its src index, so the output order
// (globally sorted by tuple) is independent of thread scheduling.

#include "core/thread_pool.hpp"
#include "ingest/ingest.hpp"
#include "model/edge_list.hpp"

namespace ftv {

// Consumes (frees) the workers' edge partials. Adds phase timings to stats.
EdgeList aggregate_edges(IngestResult& ingest, ThreadPool& pool);

}  // namespace ftv
