#include "app/live_hud.hpp"

#include <algorithm>
#include <cmath>

#include "app/hud_stats.hpp"
#include "render/font.hpp"

namespace ftv {

namespace {

// Ink #D9DCE1 at three alphas and Amber #C8962E. No other colours.
constexpr std::uint32_t kInk = pack_rgba(0xD9, 0xDC, 0xE1, 220);
constexpr std::uint32_t kInkMid = pack_rgba(0xD9, 0xDC, 0xE1, 150);
constexpr std::uint32_t kInkDim = pack_rgba(0xD9, 0xDC, 0xE1, 77);  // 30 %
constexpr std::uint32_t kAmber = pack_rgba(0xC8, 0x96, 0x2E, 230);
constexpr std::uint32_t kAmberDim = pack_rgba(0xC8, 0x96, 0x2E, 120);

template <std::size_t N>
void mmss(FixedText<N>& t, double seconds) noexcept {
  const auto s = static_cast<std::uint64_t>(std::max(0.0, seconds));
  const std::uint64_t m = s / 60, r = s % 60;
  if (m < 10) t.ch('0');
  t.u64(m).ch(':');
  if (r < 10) t.ch('0');
  t.u64(r);
}

// HH:MM:SS UTC of an epoch-milliseconds time.
template <std::size_t N>
void hhmmss(FixedText<N>& t, std::int64_t ms) noexcept {
  const std::int64_t s = ((ms / 1000) % 86400 + 86400) % 86400;
  const auto two = [&t](std::int64_t v) {
    if (v < 10) t.ch('0');
    t.u64(static_cast<std::uint64_t>(v));
  };
  two(s / 3600);
  t.ch(':');
  two((s / 60) % 60);
  t.ch(':');
  two(s % 60);
}

template <std::size_t N>
void usd(FixedText<N>& t, double v) noexcept {
  if (v < 0.0) {
    t.ch('-');
    v = -v;
  }
  t.ch('$').si(v, 2);
}

}  // namespace

LiveHudLayout LiveHud::layout(int width, int height, float feedsTop) noexcept {
  LiveHudLayout l;
  const float s = HudStats::text_scale(height);
  const float margin = 10.0f * s;
  const float adv = 6.0f * s;
  l.scale = s;
  l.panel_scale = 1.375f * std::max(1.0f, std::round(static_cast<float>(height) / 1080.0f));
  const float bw = 10.0f * adv;  // "EXPORT [E]"
  l.export_button = {static_cast<float>(width) - margin - bw, margin, bw, HudRenderer::line_height(s)};
  l.panel_right = static_cast<float>(width) - margin;
  l.panel_y = margin + 2.2f * HudRenderer::line_height(s);
  l.toast_y = margin;
  l.feeds_x = margin;
  l.feeds_y = feedsTop;
  const float tw = static_cast<float>(width) * 0.5f;
  l.timeline_y = static_cast<float>(height) - margin - 4.0f * HudRenderer::line_height(s);
  l.timeline = {(static_cast<float>(width) - tw) * 0.5f, l.timeline_y - 8.0f * s, tw, 16.0f * s};
  return l;
}

void LiveHud::refresh(const LiveSnapshot& snap, const LiveHudInput& in) noexcept {
  // ---- summary: "LIVE 7/8 STREAMS   412 TX/S" ------------------------------------
  summary_.clear();
  {
    std::size_t streams = 0, live = 0;
    bool replaying = false;
    double tps = 0.0;
    for (std::size_t f = 0; f < kFeedCount; ++f) {
      const FeedId id = static_cast<FeedId>(f);
      if (id == FeedId::Fiat) continue;
      tps += snap.feeds[f].tx_per_s;
      if (mode_ != ModelMode::Live || !feeds_ || !config_ || !config_->feeds[f].enabled) continue;
      ++streams;
      const FeedStatus& st = feeds_->status(id);
      if (st.replay.load(std::memory_order_relaxed)) replaying = true;
      if (static_cast<FeedState>(st.state.load(std::memory_order_relaxed)) == FeedState::Live) ++live;
    }
    if (mode_ == ModelMode::Live) {
      summary_.str("LIVE ").u64(live).ch('/').u64(streams).str(" STREAMS   ");
      summary_.fixed(tps, tps < 10.0 ? 1 : 0).str(" TX/S");
      if (replaying) summary_.str("   OFFLINE: REPLAY");
    } else if (mode_ == ModelMode::Replay) {
      summary_.str("REPLAY   ").fixed(tps, tps < 10.0 ? 1 : 0).str(" TX/S");
    }
  }

  // ---- feed rows: "* BTC  LIVE  184 tx/s  120 ms  drops 0" -----------------------
  for (std::size_t f = 0; f < kFeedCount; ++f) {
    Row& row = rows_[f];
    row.text.clear();
    row.dot = 0;
    const FeedId id = static_cast<FeedId>(f);
    const FeedPanelStats& fp = snap.feeds[f];
    const bool fiat = id == FeedId::Fiat;
    const bool enabled = config_ ? config_->feeds[f].enabled : fiat;
    std::string_view state;
    std::uint64_t drops = 0;
    if (fiat) {
      if (!enabled) continue;
      state = "MODELED";
      row.dot = 1;
    } else if (mode_ == ModelMode::Replay) {
      if (fp.events == 0) continue;  // not in the capture
      state = "REPLAY";
      row.dot = 3;
    } else if (mode_ == ModelMode::Live && feeds_ && enabled) {
      const FeedStatus& st = feeds_->status(id);
      const auto fs = static_cast<FeedState>(st.state.load(std::memory_order_relaxed));
      drops = st.drops.load(std::memory_order_relaxed);
      if (st.replay.load(std::memory_order_relaxed)) {
        state = "REPLAY";
        row.dot = 3;
      } else {
        state = kFeedStateNames[static_cast<std::size_t>(fs) % 5];
        if (fs == FeedState::Live) {
          // A feed whose data arrives behind the tip says so in place of LIVE.
          if (config_->feeds[f].label == SourceLabel::Delayed) state = "DELAYED";
          row.dot = 1;
        } else {
          row.dot = 2;
        }
      }
    } else {
      continue;
    }
    row.text.str("  ").str(feed_name(id)).pad_to(14).str(state).pad_to(28);
    if (fiat) {
      row.text.ch('$').si(fp.usd_per_s, 1).str("/s");
    } else if (id == FeedId::Fx) {
      // Reference rates, not flows: show the ECB date and EURUSD.
      const FxBoard* fx = feeds_ ? &feeds_->fx() : nullptr;
      const std::uint32_t d = fx ? fx->date() : 0;
      if (d) {
        row.text.str("ECB ").u64(d / 10000).ch('-');
        if ((d / 100) % 100 < 10) row.text.ch('0');
        row.text.u64((d / 100) % 100).ch('-');
        if (d % 100 < 10) row.text.ch('0');
        row.text.u64(d % 100).str("  EURUSD ").fixed(fx->usd(Asset::EUR), 4);
      } else {
        row.text.str("no rates yet");
      }
      continue;
    } else {
      row.text.fixed(fp.tx_per_s, fp.tx_per_s < 10.0 ? 1 : 0).str(" tx/s");
    }
    if (!fiat && mode_ == ModelMode::Live && feeds_ && row.dot == 1) {
      const std::int32_t lat = feeds_->status(id).latency_ms.load(std::memory_order_relaxed);
      row.text.pad_to(42);
      if (lat >= 0) {
        if (lat >= 10000) {
          row.text.fixed(lat / 1000.0, 0).str(" s");
        } else {
          row.text.u64(static_cast<std::uint64_t>(lat)).str(" ms");
        }
      } else {
        row.text.ch('-');
      }
    }
    row.text.pad_to(52).str("drops ").u64(drops);
  }

  // ---- whale tape: "12:04:31  $3.10M  BTC  BINANCE  NEW YORK > BITCOIN  LIVE" --------
  tape_lines_ = 0;
  for (std::size_t i = 0; i < snap.tape_count && i < tape_.size(); ++i) {
    const WhaleRecord& w = snap.tape[i];
    TapeLine& t = tape_[tape_lines_++];
    t.tier = w.tier;
    t.text.clear();
    hhmmss(t.text, w.ts_ms);
    t.text.str("  ");
    usd(t.text, w.usd_value);
    t.text.pad_to(19).str(asset_name(w.asset)).pad_to(25).str(feed_name(w.feed)).pad_to(37);
    auto hubName = [this](std::uint16_t h) -> std::string_view {
      if (h >= taxonomy_.size()) return "?";
      const HubDef& d = taxonomy_.hubs()[h];
      return d.display_name.empty() ? std::string_view(d.id) : std::string_view(d.display_name);
    };
    t.text.str(hubName(w.src_hub)).str(" > ").str(hubName(w.dst_hub)).str("  ").str(label_name(w.label));
  }

  // ---- hub panel ---------------------------------------------------------------
  panel_lines_ = 0;
  const int h = in.pinned_hub;
  if (h >= 0 && static_cast<std::size_t>(h) < taxonomy_.size() && static_cast<std::size_t>(h) < snap.hub_count) {
    const HubDef& d = taxonomy_.hubs()[static_cast<std::size_t>(h)];
    const HubPanelStats& st = snap.hubs[static_cast<std::size_t>(h)];
    auto line = [this]() -> FixedText<96>& {
      FixedText<96>& t = panel_[std::min(panel_lines_, panel_.size() - 1)];
      if (panel_lines_ < panel_.size()) ++panel_lines_;
      t.clear();
      return t;
    };
    line().str(d.display_name.empty() ? std::string_view(d.id) : std::string_view(d.display_name));
    {
      auto& t = line();
      bool any = false;
      for (std::size_t l = 0; l < kLabelCount; ++l) {
        if (st.label_mask & (1u << l)) {
          if (any) t.str(" + ");
          t.str(kSourceLabelNames[l]);
          any = true;
        }
      }
      if (!any) t.str("NO DATA YET");
    }
    line().str("           1M      15M  SESSION");
    {
      auto& t = line();
      t.str("IN  ");
      for (int k = 0; k < 3; ++k) {
        FixedText<16> v;
        usd(v, st.in_usd[k]);
        t.pad_to(4 + static_cast<std::size_t>(k + 1) * 9 - v.size()).str(v.view());
      }
    }
    {
      auto& t = line();
      t.str("OUT ");
      for (int k = 0; k < 3; ++k) {
        FixedText<16> v;
        usd(v, st.out_usd[k]);
        t.pad_to(4 + static_cast<std::size_t>(k + 1) * 9 - v.size()).str(v.view());
      }
    }
    {
      auto& t = line();
      t.str("NET ");
      for (int k = 0; k < 3; ++k) {
        FixedText<16> v;
        usd(v, st.in_usd[k] - st.out_usd[k]);
        t.pad_to(4 + static_cast<std::size_t>(k + 1) * 9 - v.size()).str(v.view());
      }
    }
    line().str("TX/S ").fixed(st.tx_per_s, 1).str("   WHALES 15M ").u64(st.whales_15m);
    for (int k = 0; k < 3; ++k) {
      auto& t = line();
      t.u64(static_cast<std::uint64_t>(k + 1)).str(". ");
      const int p = st.top[k];
      if (p >= 0 && static_cast<std::size_t>(p) < taxonomy_.size()) {
        const HubDef& o = taxonomy_.hubs()[static_cast<std::size_t>(p)];
        t.str(o.display_name.empty() ? std::string_view(o.id) : std::string_view(o.display_name)).str("  ");
        usd(t, st.top_usd[k]);
      } else {
        t.ch('-');
      }
    }
    line().str("[ESC] CLEAR");
  }

  // ---- timeline ------------------------------------------------------------------
  timeline_text_.clear();
  if (in.timeline_visible) {
    timeline_text_.str("REPLAY ");
    mmss(timeline_text_, in.timeline_pos_s);
    timeline_text_.str(" / ");
    mmss(timeline_text_, in.timeline_len_s);
    if (in.capture_synthetic) timeline_text_.str("  SYNTHETIC CAPTURE");
    if (in.paused) timeline_text_.str("  PAUSED");
  }
}

void LiveHud::emit(HudRenderer& hud, const LiveHudLayout& l, const LiveHudInput& in) const noexcept {
  const float s = l.scale;
  const float lh = HudRenderer::line_height(s);
  const float adv = 6.0f * s;

  // Summary, feed rows, whale tape.
  if (in.show_feeds) {
    float y = l.feeds_y;
    if (summary_.size() > 0) {
      hud.text(l.feeds_x, y, s, kInk, summary_.view());
      y += lh * 1.4f;
    }
    for (const Row& r : rows_) {
      if (r.dot == 0) continue;
      const char sym[1] = {r.dot == 3 ? kGlyphRing : kGlyphDot};
      hud.text(l.feeds_x, y, s, r.dot == 2 ? kInkDim : kInk, std::string_view(sym, 1));
      hud.text(l.feeds_x, y, s, r.dot == 2 ? kInkMid : kInk, r.text.view());
      y += lh;
    }
    if (tape_lines_ > 0) {
      y += lh * 0.8f;
      hud.text(l.feeds_x, y, s, kInkDim, "WHALE TAPE");
      y += lh;
      for (std::size_t i = 0; i < tape_lines_; ++i) {
        // Newest on top; older lines fade. Tier-3 whales in amber.
        const TapeLine& t = tape_[i];
        const std::uint32_t c = t.tier >= 3 ? (i < 3 ? kAmber : kAmberDim) : (i == 0 ? kInk : i < 4 ? kInkMid : kInkDim);
        hud.text(l.feeds_x, y, s, c, t.text.view());
        y += lh;
      }
    }
  }

  // EXPORT button (text only).
  hud.text(l.export_button.x, l.export_button.y, s, in.export_hover ? kInk : kInkMid, "EXPORT [E]");

  // Toast, centred at the top.
  if (!in.toast.empty()) {
    const float w = static_cast<float>(in.toast.size()) * adv;
    const float x = std::max(l.feeds_x, (l.panel_right + l.feeds_x - w) * 0.5f);
    hud.text(x, l.toast_y, s, kInk, in.toast);
  }

  // Hub panel, right-aligned below the button.
  if (panel_lines_ > 0) {
    const float ps = l.panel_scale;
    std::size_t maxChars = 0;
    for (std::size_t i = 0; i < panel_lines_; ++i) maxChars = std::max(maxChars, panel_[i].size());
    const float x = l.panel_right - static_cast<float>(maxChars) * 6.0f * ps;
    float y = l.panel_y;
    for (std::size_t i = 0; i < panel_lines_; ++i) {
      hud.text(x, y, ps, i == 0 ? kInk : (i == 1 || i + 1 == panel_lines_ ? kInkDim : kInkMid), panel_[i].view());
      y += HudRenderer::line_height(ps);
    }
  }

  // Timeline: a 1 px amber line, the playhead dot and the clock.
  if (in.timeline_visible && in.timeline_len_s > 0.0) {
    const float x0 = l.timeline.x, w = l.timeline.w;
    hud.panel(x0, l.timeline_y, w, 1.0f, kAmberDim);
    const auto f = static_cast<float>(std::clamp(in.timeline_pos_s / in.timeline_len_s, 0.0, 1.0));
    hud.panel(x0, l.timeline_y, w * f, 1.0f, kAmber);
    const char dot[1] = {kGlyphDot};
    hud.text(x0 + w * f - 3.0f * s, l.timeline_y - 3.5f * s, s, kAmber, std::string_view(dot, 1));
    hud.text(x0, l.timeline_y + 6.0f * s, s, kInkMid, timeline_text_.view());
  }
}

}  // namespace ftv
