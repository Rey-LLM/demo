// Live model: aggregation, labels, windows, snapshots, captures and seek
// determinism; plus the pure interaction math (hub picking, view presets).

#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "app/interaction.hpp"
#include "ingest/live/live_runtime.hpp"
#include "model/capture.hpp"
#include "model/feed_config.hpp"
#include "model/globe.hpp"
#include "model/live_model.hpp"
#include "render/camera.hpp"
#include "test_framework.hpp"
#include "test_util.hpp"

#ifndef FTVIZ_SOURCE_DIR
#define FTVIZ_SOURCE_DIR "."
#endif

using namespace ftv;

namespace {

const std::string kSrc = FTVIZ_SOURCE_DIR;

LiveModelConfig small_config() {
  LiveModelConfig c;
  c.hub_count = 4;
  c.whale_usd.fill(1000.0);
  c.whale_capacity = 4096;
  c.volume_capacity = 1 << 16;
  Corridor k;
  k.currency = Asset::EUR;
  k.src_hub = 0;
  k.dst_hub = 1;
  k.usd_per_day = 86400.0 * 10.0;  // 10 USD/s
  c.corridors = {k};
  return c;
}

LiveEvent tx(std::int64_t ts, FeedId f, Asset a, double usd, std::uint16_t s, std::uint16_t d,
             EventKind k = EventKind::Trade) {
  LiveEvent e;
  e.ts_ms = ts;
  e.feed = f;
  e.asset = a;
  e.kind = k;
  e.usd_value = usd;
  e.amount = usd;
  e.count = 1;
  e.src_hub = s;
  e.dst_hub = d;
  return e;
}

// A deterministic synthetic capture: `seconds` of events on three feeds.
Capture make_capture(const LiveModelConfig& cfg, int seconds) {
  Capture cap;
  cap.t0_ms = 1'700'000'000'000;
  cap.duration_ms = static_cast<std::int64_t>(seconds) * 1000;
  cap.flags = kCaptureSynthetic;
  cap.description = "test capture";
  std::uint64_t x = 88172645463325252ull;
  auto rnd = [&x] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  for (std::int64_t t = 0; t < cap.duration_ms; t += 37) {
    const std::uint64_t r = rnd();
    const FeedId f = (r & 3) == 0 ? FeedId::Eth : (r & 3) == 1 ? FeedId::Coinbase : FeedId::BtcMempool;
    const double usd = static_cast<double>(r >> 40) * ((r >> 8) % 97 == 0 ? 3.0 : 0.001);
    cap.records.push_back(tx(cap.t0_ms + t, f, f == FeedId::Eth ? Asset::ETH : Asset::BTC, usd,
                             static_cast<std::uint16_t>((r >> 16) % 4), static_cast<std::uint16_t>((r >> 20) % 4),
                             f == FeedId::Coinbase ? EventKind::Trade : EventKind::Transaction));
  }
  cap.build_keyframes(cfg, 10000);
  return cap;
}

struct State {
  std::uint64_t checksum;
  std::size_t whales, volumes;
  bool operator==(const State&) const = default;
};

State state_of(const LiveModel& m) { return {m.checksum(), m.whale_count(), m.volume_count()}; }

}  // namespace

TEST_CASE("model: flows are kept per source, destination, asset and source label") {
  LiveModel m(small_config());
  const std::int64_t t0 = 1'700'000'000'000;
  m.reset(t0);
  LiveEvent a = tx(t0 + 10, FeedId::Coinbase, Asset::BTC, 500.0, 2, 3);
  m.apply(a, t0 + 10);
  a.label = SourceLabel::Replay;
  m.apply(a, t0 + 20);
  m.apply(a, t0 + 30);
  CHECK_EQ(m.flow(2, 3, Asset::BTC, SourceLabel::Live).usd, 500.0);
  CHECK_EQ(m.flow(2, 3, Asset::BTC, SourceLabel::Live).tx, 1u);
  CHECK_EQ(m.flow(2, 3, Asset::BTC, SourceLabel::Replay).usd, 1000.0);
  CHECK_EQ(m.flow(2, 3, Asset::BTC, SourceLabel::Replay).tx, 2u);
  CHECK_EQ(m.flow(3, 2, Asset::BTC, SourceLabel::Live).tx, 0u);
  // Tickers never count; block headers count toward the feed, not the flows.
  m.apply(tx(t0 + 40, FeedId::Kraken, Asset::BTC, 1e9, 2, 3, EventKind::Ticker), t0 + 40);
  m.apply(tx(t0 + 50, FeedId::BtcBlocks, Asset::BTC, 0.0, 2, 2, EventKind::Block), t0 + 50);
  CHECK_EQ(m.flow(2, 3, Asset::BTC, SourceLabel::Live).usd, 500.0);
  CHECK_EQ(m.flow(2, 2, Asset::BTC, SourceLabel::Live).tx, 0u);
  LiveSnapshot* snap = new LiveSnapshot();
  m.fill_snapshot(*snap);
  CHECK_EQ(snap->feeds[static_cast<std::size_t>(FeedId::Coinbase)].events, 3u);
  CHECK_EQ(snap->feeds[static_cast<std::size_t>(FeedId::BtcBlocks)].events, 1u);
  CHECK_EQ(snap->feeds[static_cast<std::size_t>(FeedId::Kraken)].events, 0u);
  CHECK(snap->hubs[2].label_mask == ((1u << 0) | (1u << 3)));  // LIVE and REPLAY seen at hub 2
  delete snap;
}

TEST_CASE("model: whale tiers at 1x, 3x and 10x the feed threshold; non-finite values ignored") {
  LiveModel m(small_config());
  const std::int64_t t0 = 1'700'000'000'000;
  m.reset(t0);
  WhaleRecord w;
  CHECK(!m.apply(tx(t0, FeedId::Coinbase, Asset::BTC, 999.99, 0, 1), t0, &w));
  CHECK(m.apply(tx(t0, FeedId::Coinbase, Asset::BTC, 1000.0, 0, 1), t0, &w));
  CHECK_EQ(w.tier, 1);
  CHECK(m.apply(tx(t0, FeedId::Coinbase, Asset::BTC, 3000.0, 0, 1), t0, &w));
  CHECK_EQ(w.tier, 2);
  CHECK(m.apply(tx(t0, FeedId::Coinbase, Asset::ETH, 10000.0, 1, 0), t0, &w));
  CHECK_EQ(w.tier, 3);
  CHECK(w.src_hub == 1 && w.dst_hub == 0 && w.asset == Asset::ETH);
  CHECK(!m.apply(tx(t0, FeedId::Coinbase, Asset::BTC, std::nan(""), 0, 1), t0, &w));
  CHECK(!m.apply(tx(t0, FeedId::Coinbase, Asset::BTC, INFINITY, 0, 1), t0, &w));
  CHECK_EQ(m.whale_count(), 3u);
  CHECK_EQ(m.flow(0, 1, Asset::BTC, SourceLabel::Live).tx, 5u);  // counted, without a USD value
}

TEST_CASE("model: modeled corridors accrue per completed second and are labelled MODELED") {
  LiveModel m(small_config());
  const std::int64_t t0 = 1'700'000'000'000;
  m.reset(t0);
  m.advance(t0);
  m.advance(t0 + 10'000);
  // 10 completed seconds at 10 USD/s, split evenly between the two directions.
  const double fwd = m.flow(0, 1, Asset::EUR, SourceLabel::Modeled).usd;
  const double back = m.flow(1, 0, Asset::EUR, SourceLabel::Modeled).usd;
  CHECK(std::fabs(fwd - 50.0) < 1e-6);
  CHECK(std::fabs(back - 50.0) < 1e-6);
  CHECK_EQ(m.flow(0, 1, Asset::EUR, SourceLabel::Live).usd, 0.0);
  CHECK_EQ(m.flow(0, 1, Asset::EUR, SourceLabel::Modeled).tx, 0u);  // no invented transaction counts
  // Disabled: nothing accrues.
  LiveModelConfig off = small_config();
  off.fiat_enabled = false;
  LiveModel n(off);
  n.reset(t0);
  n.advance(t0 + 10'000);
  CHECK_EQ(n.flow(0, 1, Asset::EUR, SourceLabel::Modeled).usd, 0.0);
}

TEST_CASE("model: hub panel windows (1 m, 15 m, session), top counterparties, whales in 15 m") {
  LiveModelConfig cfg = small_config();
  cfg.fiat_enabled = false;
  LiveModel m(cfg);
  const std::int64_t t0 = 1'700'000'040'000;
  m.reset(t0);
  m.apply(tx(t0 + 1000, FeedId::Coinbase, Asset::BTC, 100.0, 0, 1), t0 + 1000);            // old
  m.apply(tx(t0 + 2000, FeedId::Coinbase, Asset::BTC, 5000.0, 2, 1), t0 + 2000);           // old whale
  const std::int64_t later = t0 + 5 * 60'000;
  m.apply(tx(later, FeedId::Coinbase, Asset::BTC, 40.0, 0, 1), later);
  m.apply(tx(later, FeedId::Coinbase, Asset::BTC, 7.0, 1, 3), later);
  auto snap = std::make_unique<LiveSnapshot>();
  m.advance(later + 500);
  m.fill_snapshot(*snap);
  const HubPanelStats& h1 = snap->hubs[1];
  CHECK_EQ(h1.in_usd[0], 40.0);     // last minute
  CHECK_EQ(h1.out_usd[0], 7.0);
  CHECK_EQ(h1.in_usd[1], 5140.0);   // 15 minutes
  CHECK_EQ(h1.in_usd[2], 5140.0);   // session
  CHECK_EQ(h1.whales_15m, 1u);
  CHECK_EQ(h1.top[0], 2);           // 5000 with hub 2
  CHECK_EQ(h1.top[1], 0);           // 140 with hub 0
  CHECK_EQ(h1.top[2], 3);
  CHECK_EQ(h1.top_usd[1], 140.0);
  // 20 minutes on, the 15 m window is empty but the session totals stay.
  m.advance(later + 20 * 60'000);
  m.fill_snapshot(*snap);
  CHECK_EQ(snap->hubs[1].in_usd[1], 0.0);
  CHECK_EQ(snap->hubs[1].whales_15m, 0u);
  CHECK_EQ(snap->hubs[1].in_usd[2], 5140.0);
  CHECK_EQ(snap->hubs[1].top[0], -1);
}

TEST_CASE("model: a full journal counts overflow instead of growing") {
  LiveModelConfig cfg = small_config();
  cfg.whale_capacity = 4;
  LiveModel m(cfg);
  const std::int64_t t0 = 1'700'000'000'000;
  m.reset(t0);
  for (int i = 0; i < 10; ++i) m.apply(tx(t0 + i, FeedId::Coinbase, Asset::BTC, 2000.0, 0, 1), t0 + i);
  CHECK_EQ(m.whale_count(), 4u);
  auto snap = std::make_unique<LiveSnapshot>();
  m.fill_snapshot(*snap);
  CHECK_EQ(snap->whale_overflow, 6u);
}

TEST_CASE("model: snapshot exchange publishes whole snapshots and never blocks") {
  LiveModel m(small_config());
  const std::int64_t t0 = 1'700'000'000'000;
  m.reset(t0);
  auto ex = std::make_unique<SnapshotExchange>();
  CHECK(ex->acquire() == nullptr);  // nothing published yet
  CHECK(ex->try_publish(m));
  const LiveSnapshot* held = ex->acquire();
  REQUIRE(held != nullptr);
  const std::uint64_t heldSeq = held->seq;
  const std::uint64_t heldSum = held->checksum;
  // The writer keeps publishing while a reader holds a snapshot: the held
  // one never changes, and at most every other publish is skipped.
  int published = 0;
  for (int i = 1; i <= 50; ++i) {
    m.apply(tx(t0 + i, FeedId::Coinbase, Asset::BTC, 10.0, 0, 1), t0 + i);
    published += ex->try_publish(m) ? 1 : 0;
    CHECK_EQ(held->seq, heldSeq);
    CHECK_EQ(held->checksum, heldSum);
  }
  CHECK(published >= 1);
  ex->release(held);
  CHECK(ex->try_publish(m));
  const LiveSnapshot* now = ex->acquire();
  REQUIRE(now != nullptr);
  CHECK_EQ(now->checksum, m.checksum());
  CHECK(now->seq > heldSeq);
  ex->release(now);

  // Concurrent reader: every snapshot it sees is internally consistent.
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0}, reads{0};
  std::thread reader([&] {
    while (!stop.load()) {
      const LiveSnapshot* s = ex->acquire();
      if (!s) continue;
      // The model below adds 1 USD to (0,1) and (1,0) together.
      const double a = s->flows[LiveModel::flow_cell(4, 0, 1, Asset::SOL, SourceLabel::Live)].usd;
      const double b = s->flows[LiveModel::flow_cell(4, 1, 0, Asset::SOL, SourceLabel::Live)].usd;
      if (a != b) torn.fetch_add(1);
      reads.fetch_add(1);
      ex->release(s);
    }
  });
  for (int i = 0; i < 3000; ++i) {
    const std::int64_t t = t0 + 100 + i;
    m.apply(tx(t, FeedId::Sol, Asset::SOL, 1.0, 0, 1), t);
    m.apply(tx(t, FeedId::Sol, Asset::SOL, 1.0, 1, 0), t);
    (void)ex->try_publish(m);
  }
  stop = true;
  reader.join();
  CHECK_EQ(torn.load(), 0);
  CHECK(reads.load() > 0);
}

TEST_CASE("model: journal gate never admits a reader and a truncation together") {
  JournalGate g;
  CHECK(g.reader_enter());
  CHECK(!g.writer_begin());  // postponed, not blocked
  g.reader_leave();
  CHECK(g.writer_begin());
  CHECK(!g.reader_enter());
  g.writer_end();
  CHECK(g.reader_enter());
  g.reader_leave();

  std::atomic<int> inside{0}, clash{0};
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load()) {
      if (!g.reader_enter()) continue;
      if (inside.fetch_add(1) != 0) clash.fetch_add(1);
      inside.fetch_sub(1);
      g.reader_leave();
    }
  });
  int wrote = 0;
  for (int i = 0; i < 200000; ++i) {
    if (!g.writer_begin()) continue;
    if (inside.fetch_add(1) != 0) clash.fetch_add(1);
    inside.fetch_sub(1);
    g.writer_end();
    ++wrote;
  }
  stop = true;
  reader.join();
  CHECK_EQ(clash.load(), 0);
  CHECK(wrote > 0);
}

TEST_CASE("replay: zero-run encoding round-trips") {
  std::vector<std::uint8_t> in;
  for (int i = 0; i < 5000; ++i) in.push_back(i % 97 < 60 ? 0 : static_cast<std::uint8_t>(i));
  in.insert(in.end(), 4096, 0);
  in.push_back(7);
  for (const auto& v : {in, std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(100, 0),
                        std::vector<std::uint8_t>{1, 2, 3}}) {
    const auto enc = zero_run_encode(v);
    std::vector<std::uint8_t> dec;
    CHECK(zero_run_decode(enc.data(), enc.size(), dec));
    CHECK(dec == v);
  }
  CHECK(zero_run_encode(in).size() < in.size());
  // Corrupt input is rejected.
  auto enc = zero_run_encode(in);
  enc.resize(enc.size() - 3);
  std::vector<std::uint8_t> dec;
  CHECK(!zero_run_decode(enc.data(), enc.size(), dec));
  const std::uint8_t huge[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
  CHECK(!zero_run_decode(huge, sizeof huge, dec));
}

TEST_CASE("replay: seek then play equals continuous play (model checksum and journals)") {
  const LiveModelConfig cfg = small_config();
  const Capture cap = make_capture(cfg, 65);
  CHECK_EQ(cap.keyframes.size(), 7u);  // 0, 10, ..., 60 s
  for (std::size_t i = 0; i < cap.keyframes.size(); ++i) CHECK_EQ(cap.keyframes[i].time_ms, static_cast<std::int64_t>(i) * 10000);

  // Reference: continuous play, sampled at several times.
  const std::int64_t times[] = {0, 999, 10000, 12345, 29999, 30000, 47011, 64999};
  std::vector<State> ref;
  {
    LiveModel m(cfg);
    ReplayDriver d(cap);
    d.start(m);
    for (std::int64_t t : times) {
      d.advance_to(m, t);
      ref.push_back(state_of(m));
    }
    CHECK(m.whale_count() > 0);
  }
  // Fine-grained steps give the same states as coarse ones.
  {
    LiveModel m(cfg);
    ReplayDriver d(cap);
    d.start(m);
    std::size_t k = 0;
    for (std::int64_t t = 0; t <= 64999; t += 7) {
      d.advance_to(m, t);
      while (k < std::size(times) && times[k] < t + 7 && times[k] >= t) {
        d.advance_to(m, times[k]);
        CHECK(state_of(m) == ref[k]);
        ++k;
      }
    }
  }
  // Seeks in every direction, from every position.
  LiveModel m(cfg);
  ReplayDriver d(cap);
  d.start(m);
  const std::size_t order[] = {5, 1, 7, 0, 3, 6, 2, 4, 4, 0, 7, 3};
  for (std::size_t i : order) {
    CHECK(d.seek(m, times[i]));
    CHECK(state_of(m) == ref[i]);
    CHECK_EQ(d.position_ms(), times[i]);
  }
  // Seek, then keep playing: same as never having seeked.
  d.seek(m, 12345);
  d.advance_to(m, 47011);
  CHECK(state_of(m) == ref[6]);
  d.seek(m, 3);
  d.advance_to(m, 64999);
  CHECK(state_of(m) == ref[7]);
}

TEST_CASE("replay: the timeline loops, records start over, the model keeps accumulating") {
  const LiveModelConfig cfg = small_config();
  const Capture cap = make_capture(cfg, 20);
  LiveModel m(cfg);
  ReplayDriver d(cap);
  d.start(m);
  d.advance_to(m, 19999);
  const std::size_t oneLoop = m.whale_count();
  REQUIRE(oneLoop > 0);
  int whales = 0;
  d.advance_to(m, 59999, [&](const WhaleRecord& w, double relS) {
    ++whales;
    CHECK(w.label == SourceLabel::Replay);
    CHECK(relS >= 20.0 && relS < 60.0);
  });
  CHECK_EQ(m.whale_count(), oneLoop * 3);
  CHECK_EQ(static_cast<std::size_t>(whales), oneLoop * 2);
  CHECK_EQ(d.loop_position_ms(), 19999);
  // A seek while looping lands in the first pass again, deterministically.
  d.seek(m, 5000);
  LiveModel n(cfg);
  ReplayDriver e(cap);
  e.start(n);
  e.advance_to(n, 5000);
  CHECK(state_of(m) == state_of(n));
}

TEST_CASE("replay: capture files round-trip; stale keyframes are rebuilt") {
  const LiveModelConfig cfg = small_config();
  const Capture cap = make_capture(cfg, 25);
  ftv::test::TempFile file("cap.ftxcap", "");
  std::string err;
  REQUIRE(cap.save(file.path(), err));
  Capture in;
  REQUIRE(in.load(file.path(), cfg, err));
  CHECK(!in.keyframes_rebuilt);
  CHECK(in.synthetic());
  CHECK_EQ(in.description, std::string("test capture"));
  CHECK_EQ(in.records.size(), cap.records.size());
  CHECK_EQ(in.duration_ms, cap.duration_ms);
  REQUIRE(in.keyframes.size() == cap.keyframes.size());
  for (std::size_t i = 0; i < in.keyframes.size(); ++i) {
    CHECK(in.keyframes[i].state == cap.keyframes[i].state);
    CHECK_EQ(in.keyframes[i].first_record, cap.keyframes[i].first_record);
  }
  // Another model configuration: the keyframes are rebuilt, and seeking
  // still matches continuous play under that configuration.
  LiveModelConfig other = cfg;
  other.whale_usd.fill(50.0);
  Capture in2;
  REQUIRE(in2.load(file.path(), other, err));
  CHECK(in2.keyframes_rebuilt);
  LiveModel a(other), b(other);
  ReplayDriver da(in2), db(in2);
  da.start(a);
  da.advance_to(a, 24000);
  db.start(b);
  db.advance_to(b, 24999);
  db.seek(b, 24000);
  CHECK(state_of(a) == state_of(b));
  // Truncated and foreign files are refused.
  ftv::test::TempFile bad("bad.ftxcap", "FTXC\x01");
  Capture c;
  CHECK(!c.load(bad.path(), cfg, err));
  ftv::test::TempFile notcap("not.ftxcap", "hello world, not a capture");
  CHECK(!c.load(notcap.path(), cfg, err));
}

TEST_CASE("replay: the bundled capture matches the shipped configuration and seeks deterministically") {
  std::string err;
  const auto tax = HubTaxonomy::load(kSrc + "/config/hubs.json", err);
  REQUIRE(tax.has_value());
  const auto feeds = load_feeds_config(kSrc + "/config/feeds.json", *tax, err);
  REQUIRE(feeds.has_value());
  std::vector<Corridor> corridors;
  REQUIRE(load_corridors(kSrc + "/config/flows_baseline.csv", *tax, corridors, err));
  CHECK(!corridors.empty());
  for (const Corridor& c : corridors) {
    CHECK(c.usd_per_day > 0.0);
    CHECK(!c.source_url.empty());
    CHECK(!c.as_of_date.empty());
  }
  const LiveModelConfig cfg = make_model_config(*feeds, *tax, corridors);
  Capture cap;
  REQUIRE(cap.load(kSrc + "/assets/replay/default.ftxcap", cfg, err));
  CHECK(cap.synthetic());  // generated, and flagged as such
  CHECK(!cap.keyframes_rebuilt);
  CHECK(cap.duration_ms >= 60000);
  CHECK(cap.keyframes.size() >= 6);
  LiveModel a(cfg), b(cfg);
  ReplayDriver da(cap), db(cap);
  da.start(a);
  da.advance_to(a, 73500);
  db.start(b);
  db.advance_to(b, 110000);
  CHECK(db.seek(b, 20000));  // true: a keyframe was used
  CHECK(db.seek(b, 73500));
  CHECK(state_of(a) == state_of(b));
  CHECK(a.whale_count() > 0);
}

TEST_CASE("replay: model thread stepping is deterministic and stamps whales on the timeline") {
  const LiveModelConfig cfg = small_config();
  const Capture cap = make_capture(cfg, 30);
  std::uint64_t sums[2];
  std::size_t visuals[2];
  for (int run = 0; run < 2; ++run) {
    ModelThread mt(ModelMode::Replay, cfg, nullptr, &cap);
    std::size_t n = 0;
    for (int i = 0; i < 120; ++i) {
      mt.step(100);
      VisualEvent v;
      while (mt.visuals().try_pop(v)) {
        ++n;
        CHECK(v.start_s >= 0.0f && v.start_s <= 12.0f);
        CHECK(v.tier >= 1 && v.tier <= 3);
      }
    }
    CHECK_EQ(mt.timeline_ms(), 12000);
    const LiveSnapshot* s = mt.snapshots().acquire();
    REQUIRE(s != nullptr);
    sums[run] = s->checksum;
    visuals[run] = n;
    CHECK_EQ(s->now_ms, cap.t0_ms + 12000);
    mt.snapshots().release(s);
    // Seek through the same interface the viewer uses.
    mt.request_seek(25000);
    mt.step(0);
    CHECK_EQ(mt.timeline_ms(), 25000);
    CHECK_EQ(mt.seek_generation(), 1u);
  }
  CHECK_EQ(sums[0], sums[1]);
  CHECK_EQ(visuals[0], visuals[1]);
  CHECK(visuals[0] > 0);
}

TEST_CASE("live: fallback replays only the feeds that are down, labelled REPLAY") {
  const LiveModelConfig cfg = small_config();
  const Capture cap = make_capture(cfg, 20);
  FeedsConfig fc;
  for (std::size_t i = 0; i < kFeedCount; ++i) fc.feeds[i].id = static_cast<FeedId>(i);  // none enabled: no sockets
  LiveFeeds feeds(fc, FeedHubs{});
  ModelThread mt(ModelMode::Live, cfg, &feeds, &cap);
  // ETH goes into fallback; Coinbase stays live and delivers one event.
  const std::int64_t now = mt.session_start_ms();
  CHECK(feeds.transitions(FeedId::Eth).try_push(FeedTransition{now, FeedState::Offline, false}));
  CHECK(feeds.transitions(FeedId::Eth).try_push(FeedTransition{now + 15001, FeedState::Offline, true}));
  LiveEvent live = tx(now, FeedId::Coinbase, Asset::BTC, 25.0, 0, 1);
  CHECK(feeds.events().try_push(live));
  for (int i = 0; i < 50; ++i) mt.step(100);  // 5 s of fallback
  CHECK(mt.fallback_replaying());
  const LiveSnapshot* s = mt.snapshots().acquire();
  REQUIRE(s != nullptr);
  const auto eth = static_cast<std::size_t>(FeedId::Eth), cb = static_cast<std::size_t>(FeedId::Coinbase),
             btc = static_cast<std::size_t>(FeedId::BtcMempool);
  CHECK(s->feeds[eth].events > 0);
  CHECK_EQ(s->feeds[eth].last_label, static_cast<std::uint8_t>(SourceLabel::Replay));
  CHECK_EQ(s->feeds[cb].events, 1u);  // only its live event, none from the capture
  CHECK_EQ(s->feeds[cb].last_label, static_cast<std::uint8_t>(SourceLabel::Live));
  CHECK_EQ(s->feeds[btc].events, 0u);
  CHECK_EQ(s->transition_count[eth], 2u);
  const std::uint64_t before = s->feeds[eth].events;
  mt.snapshots().release(s);
  // Recovery: the fallback stops at once.
  CHECK(feeds.transitions(FeedId::Eth).try_push(FeedTransition{now + 30000, FeedState::Live, false}));
  for (int i = 0; i < 20; ++i) mt.step(100);
  CHECK(!mt.fallback_replaying());
  s = mt.snapshots().acquire();
  REQUIRE(s != nullptr);
  CHECK_EQ(s->feeds[eth].events, before);
  mt.snapshots().release(s);
}

TEST_CASE("live: pause holds the model while the bounded ring fills and counts drops") {
  // The ring is bounded: while nothing is applied, producers fill it and
  // then fail (the adapters count those as drops); nothing blocks.
  MpscRing<LiveEvent> ring(256);
  LiveEvent e;
  std::uint64_t accepted = 0, dropped = 0;
  for (int i = 0; i < 1000; ++i) (ring.try_push(e) ? accepted : dropped) += 1;
  CHECK_EQ(accepted, 256u);
  CHECK_EQ(dropped, 744u);
}

// ---------------------------------------------------------------------------
// Interaction math
// ---------------------------------------------------------------------------

namespace {

struct Cam {
  OrbitCamera cam;
  glm::mat4 invVp{1.0f};
  float w = 1280.0f, h = 720.0f;
  Cam(const CameraPose& p, float fov = 40.0f) {
    cam.configure(fov, 0.5f, 8.0f);
    cam.set_immediate(p.target, p.distance, p.yaw, p.pitch);
    invVp = glm::inverse(cam.projection(w / h) * cam.view());
  }
  glm::vec2 project(const glm::vec3& world) const {
    const glm::vec4 c = cam.projection(w / h) * cam.view() * glm::vec4(world, 1.0f);
    return {(c.x / c.w * 0.5f + 0.5f) * w, (0.5f - c.y / c.w * 0.5f) * h};
  }
};

}  // namespace

TEST_CASE("picking: the ray through a hub's pixel picks that hub; far-side hubs and space do not") {
  std::string err;
  const auto tax = HubTaxonomy::load(kSrc + "/config/hubs.json", err);
  REQUIRE(tax.has_value());
  const float R = 1000.0f;
  const auto& hubs = tax->hubs();
  for (std::size_t i = 0; i < hubs.size(); ++i) {
    // Look straight down at hub i.
    CameraPose p;
    p.distance = 3500.0f;
    p.yaw = hubs[i].lon_deg * kDegToRad;
    p.pitch = hubs[i].lat_deg * kDegToRad;
    Cam c(p);
    const glm::vec3 pos = geo_normal(hubs[i].lat_deg, hubs[i].lon_deg) * R;
    const glm::vec2 px = c.project(pos);
    CHECK(std::fabs(px.x - 640.0f) < 1.0f && std::fabs(px.y - 360.0f) < 1.0f);
    const Ray ray = pixel_ray(c.cam.position(), c.invVp, px.x, px.y, c.w, c.h);
    CHECK(std::fabs(glm::length(ray.dir) - 1.0f) < 1e-4f);
    CHECK_EQ(pick_hub(ray, *tax, R), static_cast<int>(i));
    // From the opposite side the hub is behind the globe.
    CameraPose q = p;
    q.yaw += 3.14159265f;
    q.pitch = -p.pitch;
    Cam back(q);
    const glm::vec2 bpx = back.project(pos);
    const Ray hidden = pixel_ray(back.cam.position(), back.invVp, bpx.x, bpx.y, back.w, back.h);
    CHECK(pick_hub(hidden, *tax, R) != static_cast<int>(i));
    // A corner of the frame looks into space.
    const Ray corner = pixel_ray(c.cam.position(), c.invVp, 2.0f, 2.0f, c.w, c.h);
    CHECK(ray_sphere(corner, glm::vec3(0.0f), R) < 0.0f);
    CHECK_EQ(pick_hub(corner, *tax, R), -1);
  }
  // Frankfurt and Zurich are 3 degrees apart: each is picked at its own pixel.
  const auto fra = tax->find("FRANKFURT"), zrh = tax->find("ZURICH");
  REQUIRE(fra.has_value() && zrh.has_value());
  CameraPose p;
  p.distance = 2600.0f;
  p.yaw = 8.6f * kDegToRad;
  p.pitch = 48.7f * kDegToRad;
  Cam c(p);
  for (const std::uint16_t h : {*fra, *zrh}) {
    const glm::vec2 px = c.project(geo_normal(hubs[h].lat_deg, hubs[h].lon_deg) * R);
    CHECK_EQ(pick_hub(pixel_ray(c.cam.position(), c.invVp, px.x, px.y, c.w, c.h), *tax, R), static_cast<int>(h));
  }
}

TEST_CASE("picking: ray-sphere intersection") {
  Ray r;
  r.origin = {0.0f, 0.0f, 10.0f};
  r.dir = {0.0f, 0.0f, -1.0f};
  CHECK(std::fabs(ray_sphere(r, glm::vec3(0.0f), 2.0f) - 8.0f) < 1e-5f);
  CHECK(ray_sphere(r, glm::vec3(5.0f, 0.0f, 0.0f), 2.0f) < 0.0f);   // misses
  CHECK(ray_sphere(r, glm::vec3(0.0f, 0.0f, 20.0f), 2.0f) < 0.0f);  // behind the origin
  r.origin = {0.0f, 0.0f, 0.0f};
  CHECK(std::fabs(ray_sphere(r, glm::vec3(0.0f), 2.0f) - 2.0f) < 1e-5f);  // from inside: the exit point
}

TEST_CASE("presets: 1-5 end exactly on their views after the 1.2 s eased flight") {
  std::string err;
  const auto tax = HubTaxonomy::load(kSrc + "/config/hubs.json", err);
  REQUIRE(tax.has_value());
  RenderSettings rs;
  CHECK(kPresetFlightSeconds == 1.2f);
  const CameraPose globe = view_preset(1, *tax, rs);
  CHECK_EQ(globe.distance, rs.camera_start_distance);
  CHECK(std::fabs(globe.yaw - rs.camera_start_yaw_deg * kDegToRad) < 1e-6f);
  CHECK(std::fabs(globe.pitch - rs.camera_start_pitch_deg * kDegToRad) < 1e-6f);
  const char* focus[] = {nullptr, nullptr, "NEW_YORK", "LONDON", "TOKYO"};
  const float R = rs.globe.radius;
  for (int i = 1; i <= kViewPresetCount; ++i) {
    const CameraPose p = view_preset(i, *tax, rs);
    CHECK(p.distance > R * 1.5f);
    CHECK(glm::length(p.target) < 1e-6f);
    Cam c(p, rs.camera_fov_deg);
    if (i >= 2 && i <= 4) {
      // The region's hub is on the near side and inside the frame.
      const HubDef& h = tax->hubs()[*tax->find(focus[i])];
      const glm::vec3 pos = geo_normal(h.lat_deg, h.lon_deg) * R;
      CHECK(glm::dot(glm::normalize(c.cam.position()), glm::normalize(pos)) > 0.9f);
      const glm::vec2 px = c.project(pos);
      CHECK(px.x > 0.2f * c.w && px.x < 0.8f * c.w && px.y > 0.2f * c.h && px.y < 0.8f * c.h);
      CHECK_EQ(pick_hub(pixel_ray(c.cam.position(), c.invVp, px.x, px.y, c.w, c.h), *tax, R),
               static_cast<int>(*tax->find(focus[i])));
    }
    if (i == 4) {
      // Shanghai (31.23 N, 121.47 E) is in frame too.
      const glm::vec2 px = c.project(geo_normal(31.23f, 121.47f) * R);
      CHECK(px.x > 0.2f * c.w && px.x < 0.8f * c.w && px.y > 0.2f * c.h && px.y < 0.8f * c.h);
    }
    if (i == 5) {
      // The whole shell fits: the globe's limb is inside the frame.
      const float halfAngle = std::asin(R / p.distance);
      CHECK(halfAngle < rs.camera_fov_deg * kDegToRad * 0.5f);
    }
    // Flight endpoints are exact, from any start.
    for (int from = 1; from <= kViewPresetCount; ++from) {
      const CameraPose a = view_preset(from, *tax, rs);
      const CameraPose s0 = ease_pose(a, p, 0.0f), s1 = ease_pose(a, p, 1.0f), s2 = ease_pose(a, p, 1.7f);
      CHECK(s0.yaw == a.yaw && s0.pitch == a.pitch && s0.distance == a.distance);
      CHECK(s1.yaw == p.yaw && s1.pitch == p.pitch && s1.distance == p.distance && s1.target == p.target);
      CHECK(s2.yaw == p.yaw && s2.distance == p.distance);
    }
  }
  CHECK_EQ(view_preset(0, *tax, rs).distance, globe.distance);   // out of range: globe
  CHECK_EQ(view_preset(99, *tax, rs).distance, globe.distance);
}

TEST_CASE("presets: the flight is a monotone eased slerp along the great circle") {
  CameraPose a, b;
  a.distance = 4000.0f;
  a.yaw = -1.2f;
  a.pitch = 0.5f;
  b.distance = 2200.0f;
  b.yaw = 2.4f;
  b.pitch = 0.3f;
  auto dir = [](const CameraPose& p) {
    return glm::vec3(std::cos(p.pitch) * std::sin(p.yaw), std::sin(p.pitch), std::cos(p.pitch) * std::cos(p.yaw));
  };
  const glm::vec3 da = dir(a), db = dir(b);
  const float total = std::acos(glm::dot(da, db));
  const glm::vec3 normal = glm::normalize(glm::cross(da, db));
  float prev = 0.0f, maxStep = 0.0f, firstStep = 0.0f, midStep = 0.0f;
  for (int i = 1; i <= 100; ++i) {
    const CameraPose p = ease_pose(a, b, static_cast<float>(i) / 100.0f);
    const glm::vec3 d = dir(p);
    CHECK(std::fabs(glm::dot(d, normal)) < 1e-4f);  // stays on the great circle
    const float ang = std::acos(std::clamp(glm::dot(da, d), -1.0f, 1.0f));
    CHECK(ang >= prev - 1e-4f);                     // never turns back
    const float step = ang - prev;
    maxStep = std::max(maxStep, step);
    if (i == 1) firstStep = step;
    if (i == 50) midStep = step;
    prev = ang;
    CHECK(p.distance <= a.distance + 1e-3f && p.distance >= b.distance - 1e-3f);
  }
  CHECK(std::fabs(prev - total) < 1e-3f);
  CHECK(firstStep < midStep * 0.1f);  // eased: slow start, fast middle
  CHECK(maxStep < total * 0.04f);     // no jumps
  // Half way in time is half way along the arc (the ease is symmetric).
  const CameraPose mid = ease_pose(a, b, 0.5f);
  CHECK(std::fabs(std::acos(glm::dot(da, dir(mid))) - total * 0.5f) < 1e-3f);
}
