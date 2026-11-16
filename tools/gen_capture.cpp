// gen_capture: writes the bundled replay capture (assets/replay/default.ftxcap).
//
// The capture is SYNTHETIC: generated from a fixed seed, not recorded from
// any feed, and flagged as such in the file header (flags bit 0) and its
// description. It exists so that --replay, the live REPLAY fallback, the
// timeline and the screenshots work offline and reproducibly. Event shapes
// follow what the live adapters emit (same feeds, kinds, hubs and labels);
// sizes, rates and prices are illustrative, not market data:
//   BTC         mempool transactions, ~6/s, log-normal size (median 0.05 BTC)
//   BTC_BLOCKS  one block header at t = 61 s
//   ETH         a block every 12 s with ~140 transactions (median 0.08 ETH)
//   SOL         a block every 0.4 s, 32 sampled non-vote transactions, each
//               standing for 30 (label DELAYED, like the live adapter)
//   COINBASE    BTC/ETH/SOL trades against USD, EUR and GBP, ~14/s
//   KRAKEN      BTC/ETH/SOL trades against USD, EUR, GBP and JPY, ~8/s
//   BINANCE     BTC/ETH/SOL trades against USDT and EUR, ~16/s
//   OKX         BTC/ETH/SOL trades against USDT, ~10/s (v4)
//   BINANCE_LIQ USD-M forced liquidations, ~1 every 4 s, a few large (v4)
//   OKX         SWAP liquidation orders, ~1 every 6 s (v4)
// plus a handful of placed large trades so that every tier of whale occurs.
// On-chain transactions carry synthetic address fragments for the tape.
// The v4 streams draw from a second generator (seed + 1), so the v3 streams
// are unchanged.
// Prices: BTC 60,000, ETH 3,000, SOL 150 USD with a small random walk; EUR
// 1.08, GBP 1.27, JPY 0.0067, USDT 1.0 USD (illustrative constants).
//
// Usage: gen_capture --out assets/replay/default.ftxcap [--seed S] [--seconds N]
//                    [--hubs config/hubs.json] [--feeds config/feeds.json]
//                    [--baseline config/flows_baseline.csv] [--sessions config/sessions.json]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "model/capture.hpp"
#include "model/feed_config.hpp"
#include "model/hub_taxonomy.hpp"
#include "model/sessions.hpp"

namespace {

using namespace ftv;

struct Rng {
  std::uint64_t s[4];
  explicit Rng(std::uint64_t seed) {
    for (auto& v : s) {
      seed += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      v = z ^ (z >> 31);
    }
  }
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  std::uint64_t next() {
    const std::uint64_t r = rotl(s[1] * 5, 7) * 9;
    const std::uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
  }
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  double normal() {
    const double u1 = std::max(uniform(), 1e-300), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
  double lognormal(double median, double sigma) { return median * std::exp(sigma * normal()); }
  double exponential(double mean) { return -mean * std::log(std::max(uniform(), 1e-300)); }
};

struct Ctx {
  FeedsConfig feeds;
  std::vector<LiveEvent> out;
  std::int64_t t0 = 0;
  double px[kAssetCount] = {60000.0, 3000.0, 150.0, 1.0, 1.08, 1.27, 0.0067, 0.138, 0.128, 1.0};
  std::uint64_t ids = 1;
};

std::uint16_t chain_hub(const Ctx& c, Asset a) { return c.feeds.chain_hub[std::min<std::size_t>(static_cast<std::size_t>(a), 2)]; }

LiveEvent base_event(Ctx& c, FeedId f, EventKind k, Asset a, std::int64_t relMs) {
  LiveEvent e;
  e.ts_ms = c.t0 + relMs;
  e.feed = f;
  e.kind = k;
  e.asset = a;
  e.label = c.feeds.feeds[static_cast<std::size_t>(f)].label;
  e.count = 1;
  e.id_hash = c.ids++ * 0x9E3779B97F4A7C15ull;
  return e;
}

void chain_tx(Ctx& c, FeedId f, Asset a, std::int64_t relMs, double amount, std::uint32_t count = 1) {
  LiveEvent e = base_event(c, f, EventKind::Transaction, a, relMs);
  e.amount = amount;
  e.price = c.px[static_cast<std::size_t>(a)];
  e.usd_value = amount * e.price;
  e.count = count;
  e.src_hub = e.dst_hub = chain_hub(c, a);
  if (count > 1) e.flags |= kEvSampled;
  c.out.push_back(e);
}

void trade(Ctx& c, FeedId f, Asset base, Asset quote, bool buy, std::int64_t relMs, double usd) {
  LiveEvent e = base_event(c, f, EventKind::Trade, base, relMs);
  const double baseUsd = c.px[static_cast<std::size_t>(base)];
  e.amount = usd / baseUsd;
  e.price = baseUsd / c.px[static_cast<std::size_t>(quote)];  // quoted in the quote currency
  e.usd_value = usd;
  const std::uint16_t chain = chain_hub(c, base);
  const std::uint16_t fiat = c.feeds.quote_hub[static_cast<std::size_t>(quote)];
  e.src_hub = buy ? fiat : chain;
  e.dst_hub = buy ? chain : fiat;
  if (buy) e.flags |= kEvBuy;
  set_quote(e, quote);
  c.out.push_back(e);
}

// Forced liquidation (USD-M / SWAP): the order side closes the opposite
// position (buy: a SHORT was liquidated). Shown on the tape, valued in USD.
void liquidation(Ctx& c, FeedId f, Asset base, bool buy, std::int64_t relMs, double usd) {
  LiveEvent e = base_event(c, f, EventKind::Liquidation, base, relMs);
  const double baseUsd = c.px[static_cast<std::size_t>(base)];
  e.amount = usd / baseUsd;
  e.price = baseUsd;
  e.usd_value = usd;
  const std::uint16_t chain = chain_hub(c, base);
  const std::uint16_t fiat = c.feeds.quote_hub[static_cast<std::size_t>(Asset::USDT)];
  e.src_hub = buy ? fiat : chain;
  e.dst_hub = buy ? chain : fiat;
  if (buy) e.flags |= kEvBuy;
  set_quote(e, Asset::USDT);
  c.out.push_back(e);
}

void address_tags(Rng& rng, LiveEvent& e, bool evm) {
  static constexpr char kHex[] = "0123456789abcdef";
  static constexpr char kB58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  auto fill = [&](char* tag) {
    for (int i = 0; i < 4; ++i) {
      tag[i] = evm ? kHex[rng.next() % 16] : kB58[rng.next() % (sizeof(kB58) - 1)];
    }
  };
  fill(e.from_tag);
  fill(e.to_tag);
}

const char* arg(int argc, char** argv, const char* name, const char* def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  }
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out = arg(argc, argv, "--out", "assets/replay/default.ftxcap");
  const std::uint64_t seed = std::strtoull(arg(argc, argv, "--seed", "20261115"), nullptr, 10);
  const int seconds = std::clamp(std::atoi(arg(argc, argv, "--seconds", "120")), 20, 3600);
  const std::string hubsPath = arg(argc, argv, "--hubs", "config/hubs.json");
  const std::string feedsPath = arg(argc, argv, "--feeds", "config/feeds.json");
  const std::string basePath = arg(argc, argv, "--baseline", "config/flows_baseline.csv");
  const std::string sessionsPath = arg(argc, argv, "--sessions", "config/sessions.json");

  std::string err;
  auto tax = HubTaxonomy::load(hubsPath, err);
  if (!tax) {
    std::fprintf(stderr, "gen_capture: %s\n", err.c_str());
    return 1;
  }
  auto feeds = load_feeds_config(feedsPath, *tax, err);
  if (!feeds) {
    std::fprintf(stderr, "gen_capture: %s\n", err.c_str());
    return 1;
  }
  std::vector<Corridor> corridors;
  if (feeds->fiat_enabled && !load_corridors(basePath, *tax, corridors, err)) {
    std::fprintf(stderr, "gen_capture: %s\n", err.c_str());
    return 1;
  }
  LiveModelConfig modelCfg = make_model_config(*feeds, *tax, corridors);
  if (auto cal = load_sessions(sessionsPath, *tax, err)) {
    modelCfg.sessions = std::make_shared<const SessionCalendar>(std::move(*cal));
  } else {
    std::fprintf(stderr, "gen_capture: %s\n", err.c_str());
    return 1;
  }

  Ctx c;
  c.feeds = *feeds;
  c.t0 = 1794837600000;  // 2026-11-16T14:00:00Z, a Monday (nominal capture start: London open, New York pre-open)
  Rng rng(seed);
  const std::int64_t D = static_cast<std::int64_t>(seconds) * 1000;

  // Price random walk, one step per second, applied as events are generated
  // in time order per stream (streams are merged and sorted afterwards).
  std::vector<double> walk(static_cast<std::size_t>(seconds) + 1, 0.0);
  for (std::size_t i = 1; i < walk.size(); ++i) walk[i] = walk[i - 1] + rng.normal() * 0.0004;
  auto at = [&](std::int64_t rel) {
    const double w = std::exp(walk[static_cast<std::size_t>(std::clamp<std::int64_t>(rel / 1000, 0, seconds))]);
    c.px[0] = 60000.0 * w;
    c.px[1] = 3000.0 * w;
    c.px[2] = 150.0 * w;
  };

  // BTC mempool.
  for (double t = rng.exponential(160.0); t < static_cast<double>(D); t += rng.exponential(160.0)) {
    const auto rel = static_cast<std::int64_t>(t);
    at(rel);
    chain_tx(c, FeedId::BtcMempool, Asset::BTC, rel, rng.lognormal(0.05, 2.0));
  }
  // One block header.
  if (D > 61000) {
    LiveEvent b = base_event(c, FeedId::BtcBlocks, EventKind::Block, Asset::BTC, 61000);
    b.count = 3217;
    b.src_hub = b.dst_hub = chain_hub(c, Asset::BTC);
    c.out.push_back(b);
  }
  // ETH blocks.
  for (std::int64_t rel = 5000; rel < D; rel += 12000) {
    at(rel);
    const int n = 110 + static_cast<int>(rng.uniform() * 60.0);
    for (int i = 0; i < n; ++i) chain_tx(c, FeedId::Eth, Asset::ETH, rel, rng.lognormal(0.08, 2.4));
  }
  // SOL blocks: 32 sampled transactions, each representing 30.
  for (std::int64_t rel = 200; rel < D; rel += 400) {
    at(rel);
    for (int i = 0; i < 32; ++i) chain_tx(c, FeedId::Sol, Asset::SOL, rel, rng.lognormal(0.6, 2.2), 30);
  }
  // Exchange trades.
  struct Venue {
    FeedId feed;
    double mean_gap_ms;
    Asset quotes[4];
    double quote_w[4];
  };
  const Venue venues[3] = {
      {FeedId::Coinbase, 70.0, {Asset::USD, Asset::EUR, Asset::GBP, Asset::USD}, {0.72, 0.16, 0.12, 0.0}},
      {FeedId::Kraken, 125.0, {Asset::USD, Asset::EUR, Asset::GBP, Asset::JPY}, {0.55, 0.25, 0.12, 0.08}},
      {FeedId::Binance, 60.0, {Asset::USDT, Asset::EUR, Asset::USDT, Asset::USDT}, {0.88, 0.12, 0.0, 0.0}},
  };
  for (const Venue& v : venues) {
    for (double t = rng.exponential(v.mean_gap_ms); t < static_cast<double>(D); t += rng.exponential(v.mean_gap_ms)) {
      const auto rel = static_cast<std::int64_t>(t);
      at(rel);
      const double ub = rng.uniform();
      const Asset base = ub < 0.55 ? Asset::BTC : ub < 0.85 ? Asset::ETH : Asset::SOL;
      double uq = rng.uniform();
      Asset quote = Asset::USD;
      for (int q = 0; q < 4; ++q) {
        if (uq < v.quote_w[q]) {
          quote = v.quotes[q];
          break;
        }
        uq -= v.quote_w[q];
      }
      if (base == Asset::SOL) quote = v.feed == FeedId::Binance ? Asset::USDT : Asset::USD;
      trade(c, v.feed, base, quote, rng.uniform() < 0.5, rel, rng.lognormal(900.0, 1.9));
    }
  }
  // Placed large trades and transfers (all tiers, several hubs).
  struct Placed {
    std::int64_t rel;
    FeedId feed;
    Asset base, quote;
    bool buy;
    double usd;
  };
  const Placed placed[] = {
      {8000, FeedId::Coinbase, Asset::BTC, Asset::GBP, true, 2.6e6},   // LONDON -> BTC, tier 3
      {19000, FeedId::Kraken, Asset::ETH, Asset::EUR, false, 0.9e6},   // ETH -> FRANKFURT, tier 2
      {33000, FeedId::Coinbase, Asset::BTC, Asset::USD, false, 4.1e6}, // BTC -> NEW_YORK, tier 3
      {47000, FeedId::Kraken, Asset::BTC, Asset::JPY, true, 0.4e6},    // TOKYO -> BTC, tier 1
      {58000, FeedId::Coinbase, Asset::ETH, Asset::GBP, false, 1.1e6}, // ETH -> LONDON, tier 2
      {74000, FeedId::Kraken, Asset::BTC, Asset::GBP, true, 0.8e6},    // LONDON -> BTC, tier 2
      {90000, FeedId::Coinbase, Asset::SOL, Asset::USD, true, 0.6e6},  // NEW_YORK -> SOL, tier 1
      {103000, FeedId::Kraken, Asset::ETH, Asset::EUR, true, 3.0e6},   // FRANKFURT -> ETH, tier 3
      {25000, FeedId::Binance, Asset::BTC, Asset::USDT, true, 1.4e6},  // NEW_YORK -> BTC, tier 2
      {66000, FeedId::Binance, Asset::ETH, Asset::USDT, false, 2.9e6}, // ETH -> NEW_YORK, tier 3
  };
  for (const Placed& p : placed) {
    if (p.rel >= D) continue;
    at(p.rel);
    trade(c, p.feed, p.base, p.quote, p.buy, p.rel, p.usd);
  }
  if (D > 40000) {
    at(40000);
    chain_tx(c, FeedId::BtcMempool, Asset::BTC, 40000, 52.0);  // ~3.1 M USD on-chain, tier 3
  }

  // ---- v4 streams (second generator) ----------------------------------------
  Rng rng4(seed + 1);
  for (LiveEvent& e : c.out) {
    if (e.kind == EventKind::Transaction) address_tags(rng4, e, e.feed == FeedId::Eth);
  }
  // OKX spot trades against USDT.
  for (double t = rng4.exponential(100.0); t < static_cast<double>(D); t += rng4.exponential(100.0)) {
    const auto rel = static_cast<std::int64_t>(t);
    at(rel);
    const double ub = rng4.uniform();
    const Asset base = ub < 0.5 ? Asset::BTC : ub < 0.8 ? Asset::ETH : Asset::SOL;
    trade(c, FeedId::Okx, base, Asset::USDT, rng4.uniform() < 0.5, rel, rng4.lognormal(1100.0, 1.9));
  }
  // Liquidations: Binance USD-M and OKX SWAP.
  for (double t = rng4.exponential(4000.0); t < static_cast<double>(D); t += rng4.exponential(4000.0)) {
    const auto rel = static_cast<std::int64_t>(t);
    at(rel);
    const Asset base = rng4.uniform() < 0.6 ? Asset::BTC : Asset::ETH;
    liquidation(c, FeedId::BinanceLiq, base, rng4.uniform() < 0.45, rel, rng4.lognormal(25000.0, 1.6));
  }
  for (double t = rng4.exponential(6000.0); t < static_cast<double>(D); t += rng4.exponential(6000.0)) {
    const auto rel = static_cast<std::int64_t>(t);
    at(rel);
    const Asset base = rng4.uniform() < 0.55 ? Asset::BTC : rng4.uniform() < 0.7 ? Asset::ETH : Asset::SOL;
    liquidation(c, FeedId::Okx, base, rng4.uniform() < 0.5, rel, rng4.lognormal(18000.0, 1.6));
  }
  // Placed large liquidations (whale tiers on the liquidation tape).
  const struct {
    std::int64_t rel;
    FeedId feed;
    Asset base;
    bool buy;
    double usd;
  } placedLiq[] = {
      {14000, FeedId::BinanceLiq, Asset::BTC, false, 1.8e6},  // long liquidated, tier 2
      {52000, FeedId::Okx, Asset::ETH, true, 0.45e6},         // short liquidated, tier 1
      {81000, FeedId::BinanceLiq, Asset::ETH, true, 3.4e6},   // short liquidated, tier 3
  };
  for (const auto& p : placedLiq) {
    if (p.rel >= D) continue;
    at(p.rel);
    liquidation(c, p.feed, p.base, p.buy, p.rel, p.usd);
  }

  std::stable_sort(c.out.begin(), c.out.end(), [](const LiveEvent& a, const LiveEvent& b) { return a.ts_ms < b.ts_ms; });

  Capture cap;
  cap.records = std::move(c.out);
  cap.t0_ms = c.t0;
  cap.duration_ms = D;
  cap.flags = kCaptureSynthetic;
  char desc[160];
  std::snprintf(desc, sizeof desc, "SYNTHETIC capture from tools/gen_capture (seed %llu, %d s); not recorded market data",
                static_cast<unsigned long long>(seed), seconds);
  cap.description = desc;
  cap.build_keyframes(modelCfg, 10000);
  if (!cap.save(out, err)) {
    std::fprintf(stderr, "gen_capture: %s\n", err.c_str());
    return 1;
  }
  std::printf("gen_capture: %s: %zu records, %d s, %zu keyframes (SYNTHETIC)\n", out.c_str(), cap.records.size(),
              seconds, cap.keyframes.size());
  return 0;
}
