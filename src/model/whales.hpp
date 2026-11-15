#pragma once
// Whale events: the largest single transactions per currency (see
// ingest/whale_tracker.hpp), resolved to graph nodes and placed on the
// replay timeline.

#include <cstdint>
#include <vector>

#include "ingest/ingest.hpp"
#include "model/graph.hpp"

namespace ftv {

struct WhaleEvent {
  std::uint32_t src = 0;       // dense node index
  std::uint32_t dst = 0;
  std::uint32_t currency = 0;  // packed code
  std::uint32_t rank = 0;      // rank within its currency (0 = largest)
  std::uint32_t tier = 1;      // 1..3, drawn 2 / 3 / 4 px wide
  std::int64_t amount_minor = 0;
  std::int64_t ts_ms = 0;
  float start = 0.0f;          // replay time (s) at which the arc leaves its source
};

struct WhaleSet {
  std::vector<WhaleEvent> events;  // sorted by start time
  std::int64_t t0_ms = 0;          // dataset time range (all aggregated edges)
  std::int64_t t1_ms = 0;
  float replay_seconds = 0.0f;
};

// Tier by rank within the currency: ranks 0-1 -> 3, 2-5 -> 2, others -> 1.
std::uint32_t whale_tier(std::uint32_t rank) noexcept;

// Maps the dataset time range onto [0, replaySeconds - tailSeconds] so every
// event (flight + flash) finishes before the replay loops.
WhaleSet build_whales(const IngestResult& ingest, const Graph& graph, float replaySeconds, float tailSeconds);

}  // namespace ftv
