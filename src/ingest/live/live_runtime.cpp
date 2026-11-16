#include "ingest/live/live_runtime.hpp"

#include <algorithm>
#include <chrono>

namespace ftv {

std::int64_t utc_now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// LiveFeeds
// ---------------------------------------------------------------------------

LiveFeeds::LiveFeeds(const FeedsConfig& cfg, const FeedHubs& hubs, std::size_t ringCapacity)
    : cfg_(cfg), hubs_(hubs), ring_(ringCapacity) {
  for (std::size_t i = 0; i < kFeedCount; ++i) {
    Context& c = contexts_[i];
    c.all = &cfg_;
    c.cfg = &cfg_.feeds[i];
    c.hubs = hubs_;
    c.prices = &prices_;
    c.ring = &ring_;
    c.transitions = &transitions_[i];
    c.status = &status_[i];
    c.all_status = &status_;
    c.fx = &fx_;
    c.rotations = &rotations_;
  }
}

LiveFeeds::~LiveFeeds() { stop(); }

void LiveFeeds::start() {
  for (std::size_t i = 0; i < kFeedCount; ++i) {
    if (!cfg_.feeds[i].enabled || static_cast<FeedId>(i) == FeedId::Fiat || threads_[i].joinable()) continue;
    threads_[i] = std::jthread([this, i](std::stop_token st) { run_feed_adapter(contexts_[i], std::move(st)); });
  }
}

void LiveFeeds::stop() {
  for (auto& t : threads_) {
    if (t.joinable()) t.request_stop();
  }
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
}

// ---------------------------------------------------------------------------
// ModelThread
// ---------------------------------------------------------------------------

ModelThread::ModelThread(ModelMode mode, LiveModelConfig cfg, LiveFeeds* feeds, const Capture* capture)
    : mode_(mode), model_(std::move(cfg)), feeds_(feeds), capture_(capture) {
  if (capture_ && capture_->duration_ms > 0 && mode_ == ModelMode::Replay) {
    driver_ = std::make_unique<ReplayDriver>(*capture_);
    driver_->start(model_);
    session_start_ms_ = capture_->t0_ms;
    clock_ms_ = capture_->t0_ms;
  } else {
    session_start_ms_ = utc_now_ms();
    clock_ms_ = session_start_ms_;
    model_.reset(session_start_ms_);
    model_.advance(session_start_ms_);
  }
  (void)exchange_.try_publish(model_);
}

ModelThread::~ModelThread() { stop(); }

void ModelThread::start() {
  if (thread_.joinable()) return;
  thread_ = std::jthread([this](std::stop_token st) { run(std::move(st)); });
}

void ModelThread::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void ModelThread::push_visual(const WhaleRecord& w, bool liquidation, float startS) noexcept {
  VisualEvent v;
  v.start_s = startS;  // < 0: stamped with the render thread's live clock when it is popped
  v.src_hub = w.src_hub;
  v.dst_hub = w.dst_hub;
  v.tier = w.tier;
  v.feed = w.feed;
  v.liquidation = liquidation ? 1 : 0;
  v.usd = static_cast<float>(w.usd_value);
  if (!visual_.try_push(v)) visual_drops_.fetch_add(1, std::memory_order_relaxed);
}

void ModelThread::maybe_trail(const LiveEvent& e, std::int64_t clockMs, float startS) noexcept {
  const LiveModelConfig& c = model_.config();
  if (c.trail_max_per_s <= 0.0 || e.src_hub == e.dst_hub || !(e.usd_value >= c.trail_min_usd)) return;
  if (e.kind != EventKind::Trade && e.kind != EventKind::Transaction) return;
  if (trail_last_ms_ < 0 || clockMs < trail_last_ms_) trail_last_ms_ = clockMs;
  trail_tokens_ = std::min(c.trail_max_per_s * 0.5,  // burst: half a second's worth
                           trail_tokens_ + static_cast<double>(clockMs - trail_last_ms_) * 1e-3 * c.trail_max_per_s);
  trail_last_ms_ = clockMs;
  if (trail_tokens_ < 1.0) return;
  trail_tokens_ -= 1.0;
  WhaleRecord w;
  w.src_hub = e.src_hub;
  w.dst_hub = e.dst_hub;
  w.tier = 0;
  w.usd_value = e.usd_value;
  w.feed = e.feed;
  push_visual(w, false, startS);
}

void ModelThread::step_live(std::int64_t nowMs) noexcept {
  if (nowMs < clock_ms_) nowMs = clock_ms_;
  clock_ms_ = nowMs;
  bool anyReplay = false;
  if (feeds_) {
    for (std::size_t f = 0; f < kFeedCount; ++f) {
      FeedTransition t;
      while (feeds_->transitions(static_cast<FeedId>(f)).try_pop(t)) {
        model_.record_transition(static_cast<FeedId>(f), t);
        replaying_[f] = t.replay;
      }
      anyReplay = anyReplay || replaying_[f];
    }
    // Bounded per tick so one burst cannot stall publishing; the rest waits
    // in the ring (and a full ring drops on the producer side).
    LiveEvent e;
    WhaleRecord w;
    for (int i = 0; i < 32768 && feeds_->events().try_pop(e); ++i) {
      if (model_.apply(e, nowMs, &w)) {
        push_visual(w, e.kind == EventKind::Liquidation, -1.0f);
      } else {
        maybe_trail(e, nowMs, -1.0f);
      }
    }
  }
  fallback_any_.store(anyReplay, std::memory_order_relaxed);

  // REPLAY fallback: the capture plays on a looping 1x timeline; only the
  // records of feeds that are currently in fallback are applied.
  if (anyReplay && capture_ && capture_->duration_ms > 0 && !capture_->records.empty()) {
    const std::int64_t D = capture_->duration_ms;
    if (fb_last_ms_ < 0) fb_last_ms_ = nowMs;
    const std::int64_t target = fb_pos_ms_ + std::min<std::int64_t>(nowMs - fb_last_ms_, 5000);
    fb_last_ms_ = nowMs;
    WhaleRecord w;
    while (true) {
      const std::int64_t loopBase = fb_pos_ms_ - fb_pos_ms_ % D;
      while (fb_cursor_ < capture_->records.size()) {
        const LiveEvent& src = capture_->records[fb_cursor_];
        const std::int64_t rel = src.ts_ms - capture_->t0_ms + loopBase;
        if (rel > target) break;
        if (rel >= fb_pos_ms_ && replaying_[static_cast<std::size_t>(src.feed) % kFeedCount]) {
          LiveEvent e = src;
          e.ts_ms = nowMs;
          e.label = SourceLabel::Replay;
          if (model_.apply(e, nowMs, &w)) {
            push_visual(w, e.kind == EventKind::Liquidation, -1.0f);
          } else {
            maybe_trail(e, nowMs, -1.0f);
          }
        }
        ++fb_cursor_;
      }
      if (fb_cursor_ < capture_->records.size() || target < loopBase + D) break;
      fb_cursor_ = 0;
      fb_pos_ms_ = loopBase + D;
    }
    fb_pos_ms_ = target;
  } else {
    fb_last_ms_ = -1;  // the fallback timeline holds while no feed needs it
  }
  model_.advance(nowMs);
}

void ModelThread::step_replay(std::int64_t dtMs) noexcept {
  const std::int64_t seek = seek_ms_.load(std::memory_order_relaxed);
  if (seek >= 0 && gate_.writer_begin()) {
    driver_->seek(model_, seek);
    gate_.writer_end();
    seek_gen_.fetch_add(1, std::memory_order_relaxed);
    std::int64_t expected = seek;  // a newer request made meanwhile stays pending
    seek_ms_.compare_exchange_strong(expected, -1, std::memory_order_relaxed);
  }
  driver_->advance_to(model_, driver_->position_ms() + dtMs, [this](const LiveEvent& e, const WhaleRecord* w, double relS) {
    if (w) {
      push_visual(*w, e.kind == EventKind::Liquidation, static_cast<float>(relS));
    } else {
      maybe_trail(e, e.ts_ms, static_cast<float>(relS));
    }
  });
  clock_ms_ = capture_->t0_ms + driver_->position_ms();
  timeline_ms_.store(driver_->loop_position_ms(), std::memory_order_relaxed);
  position_ms_.store(driver_->position_ms(), std::memory_order_relaxed);
}

void ModelThread::step(std::int64_t dtMs) noexcept {
  switch (mode_) {
    case ModelMode::Live: step_live(clock_ms_ + dtMs); break;
    case ModelMode::Replay:
      if (driver_) step_replay(dtMs);
      break;
    case ModelMode::Dataset:
      clock_ms_ += dtMs;
      model_.advance(clock_ms_);
      break;
  }
  (void)exchange_.try_publish(model_);
}

void ModelThread::run(std::stop_token st) {
  using clock = std::chrono::steady_clock;
  auto last = clock::now();
  unsigned tick = 0;
  while (!st.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto now = clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (dt <= 0) continue;
    last = now;
    if (paused_.load(std::memory_order_relaxed)) {
      // Paused: nothing is applied and no clock advances; the feeds keep
      // buffering into the bounded ring and count drops once it is full.
      // A pending seek still applies (scrubbing while paused).
      if (mode_ == ModelMode::Replay && driver_ && seek_ms_.load(std::memory_order_relaxed) >= 0) {
        step_replay(0);
        (void)exchange_.try_publish(model_);
      }
      continue;
    }
    const std::uint64_t a0 = alloc_fn_ ? alloc_fn_() : 0;
    switch (mode_) {
      case ModelMode::Live:
      case ModelMode::Dataset: {
        const std::int64_t wall = utc_now_ms();
        if (mode_ == ModelMode::Live) {
          step_live(wall);
        } else {
          clock_ms_ = std::max(clock_ms_, wall);
          model_.advance(clock_ms_);
        }
        break;
      }
      case ModelMode::Replay:
        if (driver_) step_replay(std::min<std::int64_t>(dt, 250));
        break;
    }
    if ((++tick & 1u) == 0) (void)exchange_.try_publish(model_);  // 50 Hz
    if (alloc_fn_) {
      const std::uint64_t d = alloc_fn_() - a0;
      if (d) allocs_.fetch_add(d, std::memory_order_relaxed);
    }
  }
}

}  // namespace ftv
