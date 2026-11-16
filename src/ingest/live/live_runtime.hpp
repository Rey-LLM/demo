#pragma once
// Live runtime: one adapter thread per enabled feed and the model thread.
//
//   feed threads --LiveEvent (64 B)--> MpscRing --> model thread (LiveModel)
//   feed threads --FeedTransition----> SpscRing per feed --> model thread
//   model thread --VisualEvent-------> SpscRing --> render thread (whale arcs)
//   model thread --LiveSnapshot------> SnapshotExchange --> HUD (4 Hz), exporter
//
// A full ring drops the item and counts it; nothing here ever blocks on a
// consumer. Feed threads allocate only when a connection opens.
//
// Model modes:
//   Live     events from the adapters; a feed that is OFFLINE / RATE_LIMITED
//            for longer than fallback_after_s is replaced by that feed's
//            records from the bundled capture (labelled REPLAY), looped at 1x,
//            until a live message arrives again.
//   Replay   the whole capture drives the model on its own timeline (seekable).
//   Dataset  no feeds: only the MODELED fiat corridors run.
// MODELED corridors run in every mode when fiat_model is enabled.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/mpsc_ring.hpp"
#include "core/spsc_ring.hpp"
#include "ingest/live/endpoint_pool.hpp"
#include "ingest/live/feed_state.hpp"
#include "ingest/live/live_event.hpp"
#include "ingest/live/parsers.hpp"
#include "ingest/live/price_book.hpp"
#include "model/capture.hpp"
#include "model/feed_config.hpp"
#include "model/live_model.hpp"

namespace ftv {

// Written by one feed thread, read anywhere (relaxed).
struct FeedStatus {
  std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(FeedState::Connecting)};
  std::atomic<bool> replay{false};
  std::atomic<std::uint64_t> messages{0};
  std::atomic<std::uint64_t> events{0};
  std::atomic<std::uint64_t> drops{0};      // events lost to a full ring (or a full parse batch)
  std::atomic<std::uint64_t> malformed{0};
  std::atomic<std::uint64_t> reconnects{0};
  std::atomic<std::int64_t> last_message_ms{-1};
  std::atomic<std::uint8_t> last_error{0};  // net::NetError of the last failure
  // Source timestamp to receipt, smoothed (EMA over flow events that carry a
  // source time; includes clock skew between the provider and this machine).
  // -1 until the first such event.
  std::atomic<std::int32_t> latency_ms{-1};
  std::atomic<std::uint8_t> url_index{0};   // which of url / fallback_urls (or pool endpoint) is in use
  std::atomic<std::uint8_t> url_count{1};
  std::atomic<std::uint32_t> rotations{0};  // endpoint pool rotations
  std::atomic<std::int32_t> rpc_latency_ms{-1};  // pool: latency EWMA of the active endpoint
};

// Endpoint rotations of the pooled feeds (ETH, SOL), for the HUD and the
// export's session.json. Written by feed threads only when a rotation happens
// (rare), read by the exporter; a mutex is fine at that rate.
struct RotationRecord {
  std::int64_t at_ms = 0;
  FeedId feed = FeedId::Eth;
  std::uint8_t from = 0, to = 0;
  RotationReason reason = RotationReason::Start;
};
class RotationLog {
 public:
  static constexpr std::size_t kCapacity = 512;
  void push(const RotationRecord& r) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    items_[n_ % kCapacity] = r;
    ++n_;
  }
  // Copies the retained records, oldest first; returns how many were written.
  std::size_t copy(RotationRecord* out, std::size_t cap) const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    const std::size_t have = n_ < kCapacity ? n_ : kCapacity;
    const std::size_t first = n_ < kCapacity ? 0 : n_ % kCapacity;
    const std::size_t k = have < cap ? have : cap;
    for (std::size_t i = 0; i < k; ++i) out[i] = items_[(first + i) % kCapacity];
    return k;
  }

 private:
  mutable std::mutex mu_;
  std::array<RotationRecord, kCapacity> items_{};
  std::size_t n_ = 0;
};

using TransitionRing = SpscRing<FeedTransition, 64>;
using VisualRing = SpscRing<VisualEvent, 1024>;

std::int64_t utc_now_ms() noexcept;

// Adapter threads for the enabled feeds (all but FIAT).
class LiveFeeds {
 public:
  LiveFeeds(const FeedsConfig& cfg, const FeedHubs& hubs, std::size_t ringCapacity = std::size_t{1} << 16);
  ~LiveFeeds();
  LiveFeeds(const LiveFeeds&) = delete;
  LiveFeeds& operator=(const LiveFeeds&) = delete;

  void start();
  void stop();

  MpscRing<LiveEvent>& events() noexcept { return ring_; }
  TransitionRing& transitions(FeedId f) noexcept { return transitions_[static_cast<std::size_t>(f)]; }
  [[nodiscard]] const FeedStatus& status(FeedId f) const noexcept { return status_[static_cast<std::size_t>(f)]; }
  [[nodiscard]] bool enabled(FeedId f) const noexcept { return cfg_.feeds[static_cast<std::size_t>(f)].enabled; }
  PriceBook& prices() noexcept { return prices_; }
  [[nodiscard]] const FxBoard& fx() const noexcept { return fx_; }
  [[nodiscard]] const FeedsConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const RotationLog& rotations() const noexcept { return rotations_; }

  // What one adapter thread works with (pointers into this object).
  struct Context {
    const FeedsConfig* all = nullptr;
    const FeedConfig* cfg = nullptr;
    FeedHubs hubs;
    PriceBook* prices = nullptr;
    MpscRing<LiveEvent>* ring = nullptr;
    TransitionRing* transitions = nullptr;
    FeedStatus* status = nullptr;
    const std::array<FeedStatus, kFeedCount>* all_status = nullptr;  // fallback policy: is any feed LIVE?
    FxBoard* fx = nullptr;
    RotationLog* rotations = nullptr;
  };

 private:
  FeedsConfig cfg_;
  FeedHubs hubs_;
  PriceBook prices_;
  FxBoard fx_;
  RotationLog rotations_;
  MpscRing<LiveEvent> ring_;
  std::array<TransitionRing, kFeedCount> transitions_{};
  std::array<FeedStatus, kFeedCount> status_{};
  std::array<Context, kFeedCount> contexts_{};
  std::array<std::jthread, kFeedCount> threads_{};
};

// Runs one adapter until `stop` (adapters.cpp). Exposed for LiveFeeds only.
void run_feed_adapter(LiveFeeds::Context& c, std::stop_token stop);

enum class ModelMode : std::uint8_t { Live = 0, Replay, Dataset };
inline constexpr const char* kModelModeNames[] = {"live", "replay", "dataset"};

class ModelThread {
 public:
  // `feeds` is required in Live mode; `capture` in Replay mode and (optional)
  // as the fallback source in Live mode.
  ModelThread(ModelMode mode, LiveModelConfig cfg, LiveFeeds* feeds, const Capture* capture);
  ~ModelThread();
  ModelThread(const ModelThread&) = delete;
  ModelThread& operator=(const ModelThread&) = delete;

  void start();
  void stop();

  // Any thread.
  void set_paused(bool p) noexcept { paused_.store(p, std::memory_order_relaxed); }
  [[nodiscard]] bool paused() const noexcept { return paused_.load(std::memory_order_relaxed); }
  void request_seek(std::int64_t relMs) noexcept { seek_ms_.store(relMs, std::memory_order_relaxed); }
  // Replay timeline: position within the capture loop and its duration (ms).
  [[nodiscard]] std::int64_t timeline_ms() const noexcept { return timeline_ms_.load(std::memory_order_relaxed); }
  // Replay timeline position including completed loops (the clock that
  // replayed VisualEvents are stamped with, in seconds).
  [[nodiscard]] std::int64_t position_ms() const noexcept { return position_ms_.load(std::memory_order_relaxed); }
  // Bumped whenever a seek was applied (the renderer drops its live arcs).
  [[nodiscard]] std::uint32_t seek_generation() const noexcept { return seek_gen_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::int64_t timeline_duration_ms() const noexcept { return capture_ ? capture_->duration_ms : 0; }
  [[nodiscard]] bool fallback_replaying() const noexcept { return fallback_any_.load(std::memory_order_relaxed); }

  SnapshotExchange& snapshots() noexcept { return exchange_; }
  JournalGate& journal_gate() noexcept { return gate_; }
  VisualRing& visuals() noexcept { return visual_; }
  [[nodiscard]] const LiveModel& model() const noexcept { return model_; }
  [[nodiscard]] ModelMode mode() const noexcept { return mode_; }
  [[nodiscard]] std::uint64_t visual_drops() const noexcept { return visual_drops_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t allocations() const noexcept { return allocs_.load(std::memory_order_relaxed); }
  // Replay-mode transition history as exported (feeds that appear in the capture).
  [[nodiscard]] std::int64_t session_start_ms() const noexcept { return session_start_ms_; }
  // Debug builds: per-thread allocation counter used to assert zero
  // allocations per model tick (set before start()).
  using AllocCounterFn = std::uint64_t (*)() noexcept;
  void set_alloc_counter(AllocCounterFn fn) noexcept { alloc_fn_ = fn; }

  // One model step, without a thread (screenshot mode and tests): advances
  // the clock by `dtMs` of unpaused time.
  void step(std::int64_t dtMs) noexcept;

 private:
  void run(std::stop_token st);
  void push_visual(const WhaleRecord& w, float startS) noexcept;
  // A flow trail for a non-whale hub-to-hub event (token bucket on the model clock).
  void maybe_trail(const LiveEvent& e, std::int64_t clockMs, float startS) noexcept;
  void step_live(std::int64_t nowMs) noexcept;
  void step_replay(std::int64_t dtMs) noexcept;

  ModelMode mode_;
  LiveModel model_;
  LiveFeeds* feeds_;
  const Capture* capture_;
  std::unique_ptr<ReplayDriver> driver_;
  SnapshotExchange exchange_;
  JournalGate gate_;
  VisualRing visual_;
  std::jthread thread_;
  std::atomic<bool> paused_{false};
  std::atomic<std::int64_t> seek_ms_{-1};
  std::atomic<std::int64_t> timeline_ms_{0};
  std::atomic<std::int64_t> position_ms_{0};
  std::atomic<std::uint32_t> seek_gen_{0};
  std::atomic<bool> fallback_any_{false};
  std::atomic<std::uint64_t> visual_drops_{0};
  std::atomic<std::uint64_t> allocs_{0};
  std::int64_t session_start_ms_ = 0;
  std::int64_t clock_ms_ = 0;          // model clock (UTC ms, pause-adjusted in Live/Dataset)
  std::int64_t paused_total_ms_ = 0;
  // Live fallback: a looping cursor over the capture on the wall clock.
  std::size_t fb_cursor_ = 0;
  std::int64_t fb_pos_ms_ = 0;         // timeline position of the fallback cursor
  std::int64_t fb_last_ms_ = -1;
  std::array<bool, kFeedCount> replaying_{};
  AllocCounterFn alloc_fn_ = nullptr;
  double trail_tokens_ = 0.0;
  std::int64_t trail_last_ms_ = -1;
};

}  // namespace ftv
