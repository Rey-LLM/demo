#include "app/live_session.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "core/log.hpp"
#include "ingest/live/net.hpp"
#include "render/renderer.hpp"

#ifndef FTVIZ_VERSION
#define FTVIZ_VERSION "0.0.0"
#endif
#ifndef FTVIZ_GIT_SHA
#define FTVIZ_GIT_SHA "unknown"
#endif

namespace ftv {

namespace {

namespace fs = std::filesystem;

std::string resolve(const std::string& base, const std::string& rel) {
  if (rel.empty() || fs::path(rel).is_absolute()) return rel;
  std::error_code ec;
  const fs::path a = fs::path(base) / rel;
  if (fs::exists(a, ec)) return a.string();
  if (fs::exists(rel, ec)) return rel;
  return a.string();
}

}  // namespace

std::unique_ptr<LiveSession> LiveSession::create(const LiveSessionOptions& opts, const HubTaxonomy& taxonomy,
                                                 std::string& error) {
  std::unique_ptr<LiveSession> s(new LiveSession());
  s->opts_ = opts;
  s->taxonomy_ = &taxonomy;
  auto cfg = load_feeds_config(opts.feeds_path, taxonomy, error);
  if (!cfg) return nullptr;
  s->config_ = *cfg;

  std::vector<Corridor> corridors;
  if (s->config_.fiat_enabled) {
    const std::string path = resolve(opts.base_dir, s->config_.fiat_baseline);
    if (!load_corridors(path, taxonomy, corridors, error)) return nullptr;
  }
  LiveModelConfig modelCfg = make_model_config(s->config_, taxonomy, std::move(corridors));

  // The capture: required for --replay, optional as the fallback for --live.
  if (opts.mode != ModelMode::Dataset) {
    s->capture_path_ =
        !opts.capture_path.empty() ? opts.capture_path : resolve(opts.base_dir, s->config_.replay_capture);
    std::string cerr;
    if (s->capture_.load(s->capture_path_, modelCfg, cerr)) {
      s->capture_loaded_ = true;
      std::printf("[replay] %s: %zu records, %.0f s, %zu keyframes%s, %s\n", s->capture_path_.c_str(),
                  s->capture_.records.size(), static_cast<double>(s->capture_.duration_ms) / 1000.0,
                  s->capture_.keyframes.size(), s->capture_.keyframes_rebuilt ? " (rebuilt for this configuration)" : "",
                  s->capture_.synthetic() ? "SYNTHETIC" : "RECORDED");
    } else if (opts.mode == ModelMode::Replay) {
      error = cerr;
      return nullptr;
    } else {
      FTV_LOG_WARN("[live] no replay fallback: %s", cerr.c_str());
    }
  }
  if (opts.mode == ModelMode::Live) {
    FeedHubs hubs;
    for (std::size_t i = 0; i < 3; ++i) hubs.chain_hub[i] = s->config_.chain_hub[i];
    hubs.quote_hub = s->config_.quote_hub;
    s->feeds_ = std::make_unique<LiveFeeds>(s->config_, hubs);
  }
  s->model_ = std::make_unique<ModelThread>(opts.mode, std::move(modelCfg), s->feeds_.get(),
                                            s->capture_loaded_ ? &s->capture_ : nullptr);
  LiveSession* self = s.get();
  s->exporter_ = std::make_unique<ExportService>(
      opts.export_dir.empty() ? std::string(".") : opts.export_dir,
      [self](ExportInput& in) { return self->build_export(in); }, [self]() { self->finish_export(); });
  return s;
}

LiveSession::~LiveSession() { stop(); }

void LiveSession::start() {
  if (feeds_) feeds_->start();
  model_->start();
  exporter_->start();
}

void LiveSession::stop() {
  if (exporter_) exporter_->stop();
  if (model_) model_->stop();
  if (feeds_) feeds_->stop();
}

bool LiveSession::build_export(ExportInput& in) {
  pinned_ = model_->snapshots().acquire();
  if (!pinned_) return false;
  // Journals are read in place: hold off replay seeks (which truncate them)
  // for the duration of the export. A seek in progress is over within a tick.
  gate_held_ = false;
  for (int i = 0; i < 400 && !gate_held_; ++i) {
    gate_held_ = model_->journal_gate().reader_enter();
    if (!gate_held_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!gate_held_) return false;
  // A seek may have completed between the snapshot and the gate: take the
  // snapshot again now that the journals cannot shrink.
  model_->snapshots().release(pinned_);
  pinned_ = model_->snapshots().acquire();
  if (!pinned_) return false;

  const LiveSnapshot& snap = *pinned_;
  const LiveModel& m = model_->model();
  in.version = FTVIZ_VERSION;
  in.git_sha = FTVIZ_GIT_SHA;
  in.mode = kModelModeNames[static_cast<std::size_t>(opts_.mode)];
  if (capture_loaded_) {
    in.capture = capture_path_ + (capture_.synthetic() ? " (SYNTHETIC)" : " (RECORDED)");
  }
  in.start_ms = snap.session_start_ms;
  in.end_ms = snap.now_ms;
  for (const HubDef& h : taxonomy_->hubs()) in.hub_ids.push_back(h.id);
  in.corridors = m.config().corridors;
  for (std::size_t f = 0; f < kFeedCount; ++f) {
    const FeedConfig& fc = config_.feeds[f];
    const FeedPanelStats& fp = snap.feeds[f];
    const bool fiat = static_cast<FeedId>(f) == FeedId::Fiat;
    if (!fc.enabled && fp.events == 0) continue;
    if (opts_.mode != ModelMode::Live && !fiat && fp.events == 0) continue;
    ExportFeed ef;
    ef.id = std::string(feed_name(static_cast<FeedId>(f)));
    ef.url = fc.url;
    ef.configured_label = fc.label;
    ef.current_label = fc.label;
    ef.events = fp.events;
    ef.usd_total = fp.usd_total;
    if (fiat) {
      ef.state = "MODELED";
    } else if (opts_.mode == ModelMode::Replay) {
      ef.state = "REPLAY";
      ef.current_label = SourceLabel::Replay;
    } else if (feeds_) {
      const FeedStatus& st = feeds_->status(static_cast<FeedId>(f));
      ef.state = kFeedStateNames[st.state.load(std::memory_order_relaxed) % 5];
      if (st.replay.load(std::memory_order_relaxed)) ef.current_label = SourceLabel::Replay;
      ef.drops = st.drops.load(std::memory_order_relaxed);
      const FeedTransition* t = m.transitions(static_cast<FeedId>(f));
      ef.history.assign(t, t + snap.transition_count[f]);
    }
    in.feeds.push_back(std::move(ef));
  }
  in.snapshot = pinned_;
  in.whales = m.whales();
  in.whale_count = snap.whale_count;
  in.volumes = m.volumes();
  in.volume_count = snap.volume_count;
  return true;
}

void LiveSession::finish_export() {
  if (gate_held_) model_->journal_gate().reader_leave();
  gate_held_ = false;
  if (pinned_) model_->snapshots().release(pinned_);
  pinned_ = nullptr;
}

void LiveSession::pump_visuals(Renderer& renderer, float liveClock) noexcept {
  const std::uint32_t gen = model_->seek_generation();
  if (gen != seek_seen_) {
    seek_seen_ = gen;
    renderer.clear_live_arcs();
  }
  VisualEvent v;
  while (model_->visuals().try_pop(v)) {
    LiveArcSlot slot;
    slot.src_hub = v.src_hub;
    slot.dst_hub = v.dst_hub;
    slot.tier = v.tier;
    slot.used = 1;
    slot.start = v.start_s >= 0.0f ? v.start_s : liveClock;
    renderer.set_live_arc(next_slot_, slot);
    next_slot_ = (next_slot_ + 1) % Renderer::kLiveArcSlots;
  }
}

void LiveSession::print_feed_report() const {
  const LiveSnapshot* snap = model_->snapshots().acquire();
  for (std::size_t f = 0; f < kFeedCount; ++f) {
    const FeedConfig& fc = config_.feeds[f];
    const bool fiat = static_cast<FeedId>(f) == FeedId::Fiat;
    const std::uint64_t applied = snap ? snap->feeds[f].events : 0;
    if (fiat) {
      if (fc.enabled) {
        std::printf("[feed] %-10s MODELED       label MODELED  usd %.0f\n", "FIAT", snap ? snap->feeds[f].usd_total : 0.0);
      }
      continue;
    }
    if (opts_.mode == ModelMode::Replay) {
      if (applied) std::printf("[feed] %-10s REPLAY        label REPLAY   applied %" PRIu64 "\n", feed_name(static_cast<FeedId>(f)).data(), applied);
      continue;
    }
    if (!feeds_ || !fc.enabled) continue;
    const FeedStatus& st = feeds_->status(static_cast<FeedId>(f));
    const bool replay = st.replay.load(std::memory_order_relaxed);
    std::printf("[feed] %-10s %-13s label %-8s messages %" PRIu64 "  events %" PRIu64 "  applied %" PRIu64
                "  drops %" PRIu64 "  malformed %" PRIu64 "  reconnects %" PRIu64 "  last error: %s\n",
                feed_name(static_cast<FeedId>(f)).data(), kFeedStateNames[st.state.load(std::memory_order_relaxed) % 5],
                replay ? "REPLAY" : label_name(fc.label).data(), st.messages.load(std::memory_order_relaxed),
                st.events.load(std::memory_order_relaxed), applied, st.drops.load(std::memory_order_relaxed),
                st.malformed.load(std::memory_order_relaxed), st.reconnects.load(std::memory_order_relaxed),
                net::net_error_name(static_cast<net::NetError>(st.last_error.load(std::memory_order_relaxed))));
  }
  if (snap) model_->snapshots().release(snap);
}

}  // namespace ftv
