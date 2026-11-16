#pragma once
// config/feeds.json: endpoints, labels, rate limits, whale thresholds and the
// hub conventions for the live adapters (startup only).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ingest/live/endpoint_pool.hpp"
#include "ingest/live/live_event.hpp"
#include "model/hub_taxonomy.hpp"
#include "model/live_model.hpp"

namespace ftv {

struct FeedConfig {
  FeedId id = FeedId::BtcMempool;
  bool enabled = true;
  SourceLabel label = SourceLabel::Live;
  std::string url;
  std::vector<std::string> fallback_urls;  // tried in order after `url` fails to connect (DNS, TCP, TLS, HTTP)
  std::string rpc_url;  // SOL getBlock endpoint (HTTP JSON-RPC)
  std::string docs;
  std::vector<std::string> subscribe;
  bool try_pending = false;          // ETH: try newPendingTransactions, degrade gracefully if rejected
  double max_requests_per_s = 5.0;   // request budget for follow-up RPCs (ETH, SOL)
  std::uint32_t slot_lag = 32;       // SOL: request blocks this many slots behind the tip
  std::uint32_t sample_cap = 32;     // SOL: non-vote transactions kept per block
  double whale_usd = 0.0;
  double poll_s = 900.0;             // FX: seconds between reference-rate requests (>= 60)
  // ETH, SOL: ordered pool of public endpoints with per-endpoint limits. When
  // set, it replaces url / rpc_url / max_requests_per_s.
  std::vector<EndpointConfig> pool;
  std::string meta_url;              // OKX: public instruments endpoint (contract sizes), fetched at connect
  std::string ping_text;             // keep-alive text frame (OKX: "ping")
  double ping_s = 0.0;               // ... sent after this many seconds without a message (0: never)
  double tape_min_usd = 0.0;         // liquidations at or above this value go on the tape (whales always do)
};

struct FeedsConfig {
  double fallback_after_s = 15.0;
  double stale_after_s = 30.0;
  std::size_t max_frame_bytes = std::size_t{8} << 20;
  double backoff_base_s = 0.5;
  double backoff_cap_s = 30.0;
  // false (default): the capture stands in only while NO feed is LIVE, i.e.
  // the machine is offline. true: each failed feed is replaced on its own.
  bool fallback_per_feed = false;
  double trail_min_usd = 2500.0;    // flow trails: smallest hub-to-hub event drawn as an arc
  double trail_max_per_s = 40.0;    // ... and at most this many new trails per second
  std::string replay_capture = "assets/replay/default.ftxcap";
  std::array<FeedConfig, kFeedCount> feeds{};
  std::array<std::uint16_t, 3> chain_hub{};          // BTC, ETH, SOL
  std::array<std::uint16_t, kAssetCount> quote_hub{};
  bool fiat_enabled = true;
  std::string fiat_baseline = "config/flows_baseline.csv";
};

std::optional<FeedsConfig> load_feeds_config(const std::string& path, const HubTaxonomy& taxonomy, std::string& error);

// The model configuration implied by the feed configuration and corridors.
LiveModelConfig make_model_config(const FeedsConfig& feeds, const HubTaxonomy& taxonomy,
                                  std::vector<Corridor> corridors);

// Hash of everything a keyframe depends on (hub count, thresholds, corridors).
std::uint64_t model_config_hash(const LiveModelConfig& cfg) noexcept;

}  // namespace ftv
