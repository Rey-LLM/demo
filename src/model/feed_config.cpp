#include "model/feed_config.hpp"

#include <algorithm>
#include <cstring>

#include <xxhash.h>

#include "core/file_util.hpp"
#include "core/json.hpp"

namespace ftv {

namespace {

bool label_from(const std::string& s, SourceLabel& out) {
  for (std::size_t i = 0; i < kSourceLabelNames.size(); ++i) {
    if (s == kSourceLabelNames[i]) {
      out = static_cast<SourceLabel>(i);
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<FeedsConfig> load_feeds_config(const std::string& path, const HubTaxonomy& tax, std::string& error) {
  const auto text = read_text_file(path);
  if (!text) {
    error = "cannot read " + path;
    return std::nullopt;
  }
  std::string perr;
  const auto root = parse_json(*text, perr);
  if (!root) {
    error = path + ": " + perr;
    return std::nullopt;
  }
  FeedsConfig c;
  c.fallback_after_s = std::clamp(root->number_or("fallback_after_s", 15.0), 1.0, 3600.0);
  c.stale_after_s = std::clamp(root->number_or("stale_after_s", 30.0), 1.0, 3600.0);
  c.max_frame_bytes = static_cast<std::size_t>(std::clamp(root->number_or("max_frame_bytes", 8388608.0), 4096.0, 67108864.0));
  c.backoff_base_s = std::clamp(root->number_or("backoff_base_s", 0.5), 0.05, 60.0);
  c.backoff_cap_s = std::clamp(root->number_or("backoff_cap_s", 30.0), c.backoff_base_s, 600.0);
  c.replay_capture = root->string_or("replay_capture", c.replay_capture);

  auto hub = [&](const std::string& id, std::uint16_t& out) {
    const auto h = tax.find(id);
    if (!h) {
      error = path + ": unknown hub " + id;
      return false;
    }
    out = *h;
    return true;
  };
  c.quote_hub.fill(tax.fallback());
  if (const JsonValue* q = root->find("quote_hubs")) {
    for (const auto& [code, v] : q->members()) {
      Asset a;
      bool ok = false;
      for (std::size_t i = 0; i < kAssetCount; ++i) {
        if (code == kAssetNames[i]) {
          a = static_cast<Asset>(i);
          ok = true;
        }
      }
      if (!ok || !hub(v.as_string(), c.quote_hub[static_cast<std::size_t>(a)])) {
        if (error.empty()) error = path + ": bad quote_hubs entry " + code;
        return std::nullopt;
      }
    }
  }
  const char* chains[3] = {"BTC", "ETH", "SOL"};
  const JsonValue* ch = root->find("chain_hubs");
  for (std::size_t i = 0; i < 3; ++i) {
    const std::string id = ch ? ch->string_or(chains[i], "") : std::string();
    if (id.empty() || !hub(id, c.chain_hub[i])) {
      if (error.empty()) error = path + ": chain_hubs needs BTC, ETH and SOL";
      return std::nullopt;
    }
  }

  for (std::size_t i = 0; i < kFeedCount; ++i) {
    c.feeds[i].id = static_cast<FeedId>(i);
    c.feeds[i].enabled = false;
  }
  if (const JsonValue* feeds = root->find("feeds")) {
    for (const JsonValue& f : feeds->items()) {
      const std::string id = f.string_or("id", "");
      std::size_t idx = kFeedCount;
      for (std::size_t i = 0; i < kFeedCount; ++i) {
        if (id == kFeedNames[i]) idx = i;
      }
      if (idx == kFeedCount || idx == static_cast<std::size_t>(FeedId::Fiat)) {
        error = path + ": unknown feed id " + id;
        return std::nullopt;
      }
      FeedConfig& fc = c.feeds[idx];
      fc.enabled = f.bool_or("enabled", true);
      if (!label_from(f.string_or("label", "LIVE"), fc.label) || fc.label == SourceLabel::Modeled ||
          fc.label == SourceLabel::Replay) {
        error = path + ": feed " + id + " label must be LIVE or DELAYED";
        return std::nullopt;
      }
      fc.url = f.string_or("url", "");
      fc.rpc_url = f.string_or("rpc_url", "");
      fc.docs = f.string_or("docs", "");
      if (const JsonValue* sub = f.find("subscribe")) {
        for (const JsonValue& s : sub->items()) fc.subscribe.push_back(s.as_string());
      }
      fc.try_pending = f.bool_or("try_pending", false);
      fc.max_requests_per_s = std::clamp(f.number_or("max_requests_per_s", 5.0), 0.1, 5.0);  // never above 5 req/s
      fc.slot_lag = static_cast<std::uint32_t>(std::clamp(f.number_or("slot_lag", 32.0), 0.0, 10000.0));
      fc.sample_cap = static_cast<std::uint32_t>(std::clamp(f.number_or("sample_cap", 32.0), 1.0, 4096.0));
      fc.whale_usd = std::max(0.0, f.number_or("whale_usd", 0.0));
      if (fc.url.rfind("wss://", 0) != 0 || (!fc.rpc_url.empty() && fc.rpc_url.rfind("https://", 0) != 0)) {
        error = path + ": feed " + id + " must use wss:// (and https:// for rpc_url): TLS is mandatory";
        return std::nullopt;
      }
    }
  }
  FeedConfig& fiat = c.feeds[static_cast<std::size_t>(FeedId::Fiat)];
  fiat.label = SourceLabel::Modeled;
  if (const JsonValue* fm = root->find("fiat_model")) {
    c.fiat_enabled = fm->bool_or("enabled", true);
    c.fiat_baseline = fm->string_or("baseline", c.fiat_baseline);
  }
  fiat.enabled = c.fiat_enabled;
  return c;
}

LiveModelConfig make_model_config(const FeedsConfig& feeds, const HubTaxonomy& tax, std::vector<Corridor> corridors) {
  LiveModelConfig m;
  m.hub_count = tax.size();
  for (std::size_t i = 0; i < kFeedCount; ++i) m.whale_usd[i] = feeds.feeds[i].whale_usd;
  m.corridors = std::move(corridors);
  m.fiat_enabled = feeds.fiat_enabled;
  return m;
}

std::uint64_t model_config_hash(const LiveModelConfig& cfg) noexcept {
  XXH3_state_t st;
  XXH3_64bits_reset(&st);
  const std::uint64_t hubs = cfg.hub_count;
  XXH3_64bits_update(&st, &hubs, sizeof(hubs));
  XXH3_64bits_update(&st, cfg.whale_usd.data(), sizeof(double) * cfg.whale_usd.size());
  const std::uint8_t fiat = cfg.fiat_enabled ? 1 : 0;
  XXH3_64bits_update(&st, &fiat, 1);
  for (const Corridor& c : cfg.corridors) {
    XXH3_64bits_update(&st, &c.currency, sizeof(c.currency));
    XXH3_64bits_update(&st, &c.src_hub, sizeof(c.src_hub));
    XXH3_64bits_update(&st, &c.dst_hub, sizeof(c.dst_hub));
    XXH3_64bits_update(&st, &c.usd_per_day, sizeof(c.usd_per_day));
  }
  return XXH3_64bits_digest(&st);
}

}  // namespace ftv
