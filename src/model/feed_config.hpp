#pragma once
// config/feeds.json: endpoints, labels, rate limits, whale thresholds and the
// hub conventions for the live adapters (startup only).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ingest/live/live_event.hpp"
#include "model/hub_taxonomy.hpp"
#include "model/live_model.hpp"

namespace ftv {

struct FeedConfig {
  FeedId id = FeedId::BtcMempool;
  bool enabled = true;
  SourceLabel label = SourceLabel::Live;
  std::string url;
  std::string rpc_url;  // SOL getBlock endpoint (HTTP JSON-RPC)
  std::string docs;
  std::vector<std::string> subscribe;
  bool try_pending = false;          // ETH: try newPendingTransactions, degrade gracefully if rejected
  double max_requests_per_s = 5.0;   // request budget for follow-up RPCs (ETH, SOL)
  std::uint32_t slot_lag = 32;       // SOL: request blocks this many slots behind the tip
  std::uint32_t sample_cap = 32;     // SOL: non-vote transactions kept per block
  double whale_usd = 0.0;
};

struct FeedsConfig {
  double fallback_after_s = 15.0;
  double stale_after_s = 30.0;
  std::size_t max_frame_bytes = std::size_t{8} << 20;
  double backoff_base_s = 0.5;
  double backoff_cap_s = 30.0;
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
