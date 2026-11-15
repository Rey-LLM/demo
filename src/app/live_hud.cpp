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
  // ---- feed rows: "* BTC  LIVE  184 tx/s  drops 0" ----------------------------
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
    } else {
      row.text.fixed(fp.tx_per_s, fp.tx_per_s < 10.0 ? 1 : 0).str(" tx/s");
    }
    row.text.pad_to(42).str("drops ").u64(drops);
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

  // Feed rows.
  if (in.show_feeds) {
    float y = l.feeds_y;
    for (const Row& r : rows_) {
      if (r.dot == 0) continue;
      const char sym[1] = {r.dot == 3 ? kGlyphRing : kGlyphDot};
      hud.text(l.feeds_x, y, s, r.dot == 2 ? kInkDim : kInk, std::string_view(sym, 1));
      hud.text(l.feeds_x, y, s, r.dot == 2 ? kInkMid : kInk, r.text.view());
      y += lh;
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
