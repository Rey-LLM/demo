#pragma once
// LiveModel: the model-thread aggregate of every live, replayed and modeled
// event. It owns
//   * the session hub-flow matrix (src hub, dst hub, asset, source label);
//   * rolling hub windows for the hub panel: 60 one-second buckets (1 m) and
//     15 one-minute buckets (15 m), plus session totals;
//   * per-feed one-second counters (HUD rates);
//   * append-only journals for the export: whales, 1-second volume buckets
//     per feed and label, and feed state transitions.
//
// All storage is sized at construction. apply()/advance() never allocate, so
// the model thread meets the zero-allocations-per-tick rule (asserted in
// Debug builds). The model is a pure function of the events and clock values
// it is given, which makes replay seeking deterministic: save_state() /
// load_state() capture everything except the journals, which a keyframe
// records only as lengths.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ingest/live/feed_state.hpp"
#include "ingest/live/live_event.hpp"
#include "model/sessions.hpp"

namespace ftv {

inline constexpr std::size_t kLabelCount = 4;
inline constexpr std::size_t kMaxModelHubs = 16;  // hubs.json holds 12; extra hubs share the last index

struct Corridor {
  Asset currency = Asset::USD;
  std::uint16_t src_hub = 0, dst_hub = 0;
  double usd_per_day = 0.0;
  double share_pct = 0.0;
  std::string source_url;
  std::string as_of_date;
};

struct LiveModelConfig {
  std::size_t hub_count = 12;
  std::array<double, kFeedCount> whale_usd{};  // 0 disables whales for a feed
  std::vector<Corridor> corridors;             // MODELED fiat corridors (FIAT feed)
  bool fiat_enabled = true;
  std::size_t whale_capacity = 1u << 17;
  std::size_t volume_capacity = 1u << 20;      // (feed, label, second) rows
  std::size_t transition_capacity = 256;       // per feed
  // Flow trails: hub-to-hub events below the whale threshold that still get
  // a (dim, deep-amber) arc. Visual only; not part of model_config_hash.
  double trail_min_usd = 2500.0;
  double trail_max_per_s = 40.0;
  // Tape: liquidations at or above this value per feed (whales always).
  std::array<double, kFeedCount> tape_min_usd{};
  // Trading sessions: MODELED corridor intensity = BIS daily mean x
  // mean(session factor of both ends). Null: flat intensity (v3).
  std::shared_ptr<const SessionCalendar> sessions;
};

// One row of the whale & liquidation tape.
struct TapeEntry {
  std::int64_t ts_ms = 0;
  double usd_value = 0.0;
  double amount = 0.0;
  double price = 0.0;
  FeedId feed = FeedId::BtcMempool;
  EventKind kind = EventKind::Transaction;
  Asset asset = Asset::BTC;
  std::uint8_t quote_code = 0;   // Asset + 1 (0 unknown)
  SourceLabel label = SourceLabel::Live;
  std::uint8_t tier = 0;         // whale tier 1..3 (0: a liquidation below the whale threshold)
  std::uint8_t flags = 0;        // LiveEvent flags (kEvBuy)
  std::uint8_t pad0 = 0;
  std::uint16_t src_hub = 0, dst_hub = 0;
  char from_tag[4] = {};
  char to_tag[4] = {};
  std::uint64_t id_hash = 0;
};

struct WhaleRecord {
  std::int64_t ts_ms = 0;
  double usd_value = 0.0;
  FeedId feed = FeedId::BtcMempool;
  Asset asset = Asset::BTC;
  SourceLabel label = SourceLabel::Live;
  std::uint8_t tier = 1;
  std::uint16_t src_hub = 0, dst_hub = 0;
};

struct VolumeRecord {
  std::int64_t second = 0;  // UTC epoch seconds
  FeedId feed = FeedId::BtcMempool;
  SourceLabel label = SourceLabel::Live;
  std::uint16_t pad = 0;  // explicit: records are hashed and written as bytes
  std::uint32_t tx_count = 0;
  double usd_volume = 0.0;
};
static_assert(sizeof(VolumeRecord) == 24);

struct FlowCell {
  double usd = 0.0;
  std::uint64_t tx = 0;
};

// Hub panel figures, computed at publish time.
struct HubPanelStats {
  double in_usd[3] = {0, 0, 0};   // 1 m, 15 m, session
  double out_usd[3] = {0, 0, 0};
  double tx_per_s = 0.0;          // over the last 60 s
  std::uint32_t whales_15m = 0;
  std::uint8_t label_mask = 0;    // bit per SourceLabel seen in this hub's flows
  std::int16_t top[3] = {-1, -1, -1};  // top counterparty hubs over 15 m (by |in| + |out|)
  double top_usd[3] = {0, 0, 0};
};

struct FeedPanelStats {
  double tx_per_s = 0.0;    // transactions or trades per second, last 10 s
  double usd_per_s = 0.0;
  std::uint64_t events = 0;
  double usd_total = 0.0;
  std::uint8_t last_label = 0;
};

inline constexpr std::size_t kMaxFlowCells = 16 * 16 * kAssetCount * kLabelCount;

// Immutable view published by the model thread (double-buffered).
struct LiveSnapshot {
  // Session hub-flow matrix, index ((src * hub_count + dst) * kAssetCount + asset) * kLabelCount + label.
  std::array<FlowCell, kMaxFlowCells> flows{};
  std::uint64_t seq = 0;
  std::int64_t now_ms = 0;
  std::int64_t session_start_ms = 0;
  std::size_t hub_count = 0;
  std::array<HubPanelStats, kMaxModelHubs> hubs{};
  std::array<FeedPanelStats, kFeedCount> feeds{};
  std::size_t whale_count = 0;       // journal prefix lengths at publish time
  std::size_t volume_count = 0;
  std::array<std::size_t, kFeedCount> transition_count{};
  std::uint64_t whale_overflow = 0;
  std::uint64_t volume_overflow = 0;
  std::uint64_t checksum = 0;
  // The HUD's whale & liquidation tape: the newest rows, newest first.
  static constexpr std::size_t kTapeLength = 64;  // the HUD shows 12 that pass its filter
  std::array<TapeEntry, kTapeLength> tape{};
  std::size_t tape_count = 0;
  // The tape ring's valid window at publish time: indices [tape_first, tape_total).
  std::uint64_t tape_first = 0;
  std::uint64_t tape_total = 0;
  std::uint64_t liquidations = 0;  // session count (all sizes)
};

class LiveModel {
 public:
  explicit LiveModel(LiveModelConfig cfg);

  void reset(std::int64_t sessionStartMs);

  // Applies one event at model time `nowMs` (arrival / replay time; buckets
  // use it). Whales go to the journal and, if `visual` is given, are written
  // there (returns true when a whale was produced).
  bool apply(const LiveEvent& e, std::int64_t nowMs, WhaleRecord* visual = nullptr) noexcept;
  // Advances the clock: closes finished one-second buckets and runs the
  // modeled fiat corridors for every whole second crossed (at most 900).
  void advance(std::int64_t nowMs) noexcept;
  void record_transition(FeedId feed, const FeedTransition& t) noexcept;

  void fill_snapshot(LiveSnapshot& out) const noexcept;
  [[nodiscard]] std::uint64_t checksum() const noexcept;
  [[nodiscard]] static std::size_t flow_cell(std::size_t hubCount, std::size_t s, std::size_t d, Asset a,
                                             SourceLabel l) noexcept {
    return ((s * hubCount + d) * kAssetCount + static_cast<std::size_t>(a)) * kLabelCount + static_cast<std::size_t>(l);
  }

  // Journals (append-only; entries below a published length never change
  // until truncate_journals()).
  [[nodiscard]] const WhaleRecord* whales() const noexcept { return whales_.get(); }
  [[nodiscard]] const VolumeRecord* volumes() const noexcept { return volumes_.get(); }
  [[nodiscard]] const FeedTransition* transitions(FeedId f) const noexcept {
    return transitions_.get() + static_cast<std::size_t>(f) * cfg_.transition_capacity;
  }
  [[nodiscard]] std::size_t whale_count() const noexcept { return whale_n_; }
  [[nodiscard]] std::size_t volume_count() const noexcept { return volume_n_; }
  [[nodiscard]] const FlowCell& flow(std::size_t src, std::size_t dst, Asset a, SourceLabel l) const noexcept {
    return flows_[flow_index(src, dst, static_cast<std::size_t>(a), static_cast<std::size_t>(l))];
  }
  [[nodiscard]] const LiveModelConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] std::int64_t now_ms() const noexcept { return now_ms_; }
  [[nodiscard]] std::int64_t session_start_ms() const noexcept { return session_start_ms_; }

  // Keyframes for replay seeking: the full aggregate state, without the
  // journals (their lengths are stored and restored).
  void save_state(std::vector<std::uint8_t>& out) const;
  bool load_state(const std::uint8_t* data, std::size_t size) noexcept;
  void truncate_journals(std::size_t whales, std::size_t volumes, std::uint64_t tape = ~std::uint64_t{0}) noexcept;

  // Tape ring: kTapeCapacity slots, of which the newest kTapeWindow are the
  // tape (export and HUD). The 6384-slot margin lets a reader copy the window
  // of a published snapshot while the model keeps appending.
  static constexpr std::size_t kTapeCapacity = 16384;
  static constexpr std::size_t kTapeWindow = 10000;
  [[nodiscard]] const TapeEntry& tape_at(std::uint64_t index) const noexcept { return tape_[index % kTapeCapacity]; }
  [[nodiscard]] std::uint64_t tape_total() const noexcept { return tape_n_; }
  [[nodiscard]] std::uint64_t tape_first() const noexcept {
    const std::uint64_t w = tape_n_ > kTapeWindow ? tape_n_ - kTapeWindow : 0;
    return w > tape_floor_ ? w : tape_floor_;
  }
  // Highest index ever written + 1 (release); readers re-check it after copying.
  [[nodiscard]] std::uint64_t tape_written() const noexcept { return tape_written_.load(std::memory_order_acquire); }

 private:
  struct SecBucket {
    std::int64_t second = -1;
    std::array<double, kMaxModelHubs> in{}, out{};
    std::array<std::uint32_t, kMaxModelHubs> tx{};
  };
  struct MinBucket {
    std::int64_t minute = -1;
    std::array<double, kMaxModelHubs> in{}, out{};
    std::array<std::uint32_t, kMaxModelHubs> tx{}, whales{};
  };
  struct FeedSec {
    std::int64_t second = -1;
    std::array<double, kFeedCount> usd{};
    std::array<std::uint32_t, kFeedCount> tx{};
    std::uint32_t pad = 0;  // explicit (the state is hashed as bytes)
  };
  struct VolAcc {  // no padding: saved in keyframes as bytes
    double usd = 0.0;
    std::uint32_t tx = 0;
    std::uint32_t dirty = 0;
  };

  [[nodiscard]] std::size_t flow_index(std::size_t s, std::size_t d, std::size_t a, std::size_t l) const noexcept {
    return ((s * cfg_.hub_count + d) * kAssetCount + a) * kLabelCount + l;
  }
  SecBucket& sec_bucket(std::int64_t second) noexcept;
  MinBucket& min_bucket(std::int64_t minute) noexcept;
  FeedSec& feed_sec(std::int64_t second) noexcept;
  double* pair_row(std::int64_t minute) noexcept;  // H*H doubles of that minute's bucket
  void flush_volume(std::int64_t second) noexcept;
  void run_corridors(std::int64_t second) noexcept;
  std::uint8_t whale_tier(FeedId f, double usd) const noexcept;
  void push_tape(const LiveEvent& e, double usd, std::uint8_t tier, std::size_t s, std::size_t d) noexcept;

  LiveModelConfig cfg_;
  std::int64_t session_start_ms_ = 0;
  std::int64_t now_ms_ = 0;
  std::int64_t last_second_ = -1;     // last second whose buckets are closed
  std::int64_t corridor_second_ = -1; // last second the fiat model ran for

  // --- aggregate state (saved in keyframes) ---
  std::vector<FlowCell> flows_;
  std::array<SecBucket, 60> sec_{};
  std::array<MinBucket, 15> min_{};
  std::vector<double> pairs_;            // 15 minutes * H * H
  std::array<FeedSec, 60> feed_sec_{};
  std::array<double, kMaxModelHubs> sess_in_{}, sess_out_{};
  std::array<std::uint64_t, kMaxModelHubs> sess_tx_{}, sess_whales_{};
  std::array<std::uint8_t, kMaxModelHubs> hub_labels_{};
  std::array<std::uint64_t, kFeedCount> feed_events_{};
  std::array<double, kFeedCount> feed_usd_{};
  std::array<std::uint8_t, kFeedCount> feed_label_{};
  std::array<VolAcc, kFeedCount * kLabelCount> vol_acc_{};
  std::int64_t vol_second_ = -1;
  std::uint64_t whale_overflow_ = 0, volume_overflow_ = 0;

  // --- journals ---
  std::unique_ptr<WhaleRecord[]> whales_;
  std::unique_ptr<VolumeRecord[]> volumes_;
  std::unique_ptr<FeedTransition[]> transitions_;
  std::size_t whale_n_ = 0, volume_n_ = 0;
  std::array<std::size_t, kFeedCount> transition_n_{};
  std::unique_ptr<TapeEntry[]> tape_;
  std::uint64_t tape_n_ = 0, tape_floor_ = 0, liquidations_ = 0;
  std::atomic<std::uint64_t> tape_written_{0};
};

// Double-buffered snapshot hand-off from the model thread to readers (HUD at
// 4 Hz, exporter). Readers never block the writer and the writer never
// blocks: when a reader still holds the back slot, that publish is skipped
// and retried on the next tick.
class SnapshotExchange {
 public:
  // Writer (model thread).
  bool try_publish(const LiveModel& model) noexcept;
  // Reader: pins the front slot. Returns nullptr only before the first publish.
  const LiveSnapshot* acquire() noexcept;
  void release(const LiveSnapshot* s) noexcept;

 private:
  std::array<LiveSnapshot, 2> slots_{};
  std::atomic<std::uint32_t> front_{0};
  std::array<std::atomic<std::uint32_t>, 2> readers_{};
  std::atomic<bool> published_{false};
  std::uint64_t seq_ = 0;
};

// Guards journal truncation (replay seeks) against readers (the exporter).
// Dekker-style with seq_cst: a reader announces itself, then checks for a
// pending truncation; the writer announces the truncation, then checks for
// readers. Neither side waits: the reader retries later, the writer
// postpones the seek to its next tick.
class JournalGate {
 public:
  bool reader_enter() noexcept {
    readers_.fetch_add(1, std::memory_order_seq_cst);
    if (!truncating_.load(std::memory_order_seq_cst)) return true;
    readers_.fetch_sub(1, std::memory_order_seq_cst);
    return false;
  }
  void reader_leave() noexcept { readers_.fetch_sub(1, std::memory_order_seq_cst); }
  bool writer_begin() noexcept {
    truncating_.store(true, std::memory_order_seq_cst);
    if (readers_.load(std::memory_order_seq_cst) == 0) return true;
    truncating_.store(false, std::memory_order_seq_cst);
    return false;
  }
  void writer_end() noexcept { truncating_.store(false, std::memory_order_seq_cst); }

 private:
  std::atomic<int> readers_{0};
  std::atomic<bool> truncating_{false};
};

// Loads config/flows_baseline.csv (comment lines start with '#'). Hub ids are
// resolved through `hubIndex`; rows with unknown hubs or currencies fail.
class HubTaxonomy;
bool load_corridors(const std::string& path, const HubTaxonomy& taxonomy, std::vector<Corridor>& out,
                    std::string& error);

}  // namespace ftv
