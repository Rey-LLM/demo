#include "app/live_hud.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "app/hud_stats.hpp"
#include "ingest/live/parsers.hpp"
#include "render/font.hpp"

namespace ftv {

namespace {

// Ink #D9DCE1 at three alphas and Amber #C8962E. No other colours.
constexpr std::uint32_t kInk = pack_rgba(0xD9, 0xDC, 0xE1, 220);
constexpr std::uint32_t kInkMid = pack_rgba(0xD9, 0xDC, 0xE1, 150);
constexpr std::uint32_t kInkDim = pack_rgba(0xD9, 0xDC, 0xE1, 77);  // 30 %
constexpr std::uint32_t kAmber = pack_rgba(0xC8, 0x96, 0x2E, 230);
constexpr std::uint32_t kAmberDim = pack_rgba(0xC8, 0x96, 0x2E, 120);
// Neon Gold #FFC23D: only the tape's tier / LIQ marker (whales and liquidations).
constexpr std::uint32_t kNeonGold = pack_rgba(0xFF, 0xC2, 0x3D, 240);

// "$3.4M", "$24.3M", "$812K".
template <std::size_t N>
void usd_short(FixedText<N>& t, double v) noexcept {
  t.ch('$');
  if (v >= 1e9) {
    t.fixed(v / 1e9, 1).ch('B');
  } else if (v >= 1e6) {
    t.fixed(v / 1e6, 1).ch('M');
  } else if (v >= 1e3) {
    t.fixed(v / 1e3, 0).ch('K');
  } else {
    t.fixed(v, 0);
  }
}

// "0x3f..a1" / "bc..9q" (fragments only).
template <std::size_t N>
void tag(FixedText<N>& t, const char (&f)[4], bool hex) noexcept {
  if (!f[0]) {
    t.ch('?');
    return;
  }
  if (hex) t.str("0x");
  t.ch(f[0]).ch(f[1]).ch(kGlyphEllipsis).ch(f[2]).ch(f[3]);
}

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
  l.tape_row_h = HudRenderer::line_height(s);
  l.tape_right = static_cast<float>(width) - margin;
  // 12 rows + header + chip line above the timeline area.
  l.tape_y = l.timeline_y - 2.5f * HudRenderer::line_height(s) - 14.0f * l.tape_row_h;
  l.bar_w = std::min(static_cast<float>(width) * 0.42f, 64.0f * adv);
  l.bar_x = (static_cast<float>(width) - l.bar_w) * 0.5f;
  l.bar_y = margin + 3.0f * HudRenderer::line_height(s);
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
        state = kFeedStateNames[static_cast<std::size_t>(fs) % 6];
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

  // ---- whale & liquidation tape (newest first, filtered) ----------------------------
  //   "14:02:11  OKX  LIQ  BTC-USDT  SHORT  $3.4M"
  //   "14:02:09  ETH  WHALE T2  $24.3M  0x3f..a1 -> 0x9c..07"
  {
    const std::size_t prev = tape_lines_;
    const std::uint64_t prevNewest = tape_newest_;
    tape_lines_ = 0;
    std::uint64_t newest = 0;
    for (std::size_t i = 0; i < snap.tape_count && tape_lines_ < kTapeRows; ++i) {
      const TapeEntry& w = snap.tape[i];
      const bool liq = w.kind == EventKind::Liquidation;
      if (in.filter && !in.filter->pass(w.usd_value, w.tier, w.feed, liq)) continue;
      if (tape_lines_ == 0) newest = snap.tape_total - i;  // tape index + 1 of the newest shown row
      TapeLine& t = tape_[tape_lines_++];
      t.head.clear();
      t.tail.clear();
      t.marker.clear();
      hhmmss(t.head, w.ts_ms);
      t.head.str("  ").str(kFeedVenues[static_cast<std::size_t>(w.feed) % kFeedCount]).str("  ");
      const Asset q = w.quote_code > 0 && w.quote_code <= kAssetCount ? static_cast<Asset>(w.quote_code - 1)
                                                                        : Asset::Count;
      if (liq) {
        t.marker.str("LIQ");
        t.tail.str("  ").str(asset_name(w.asset));
        if (q != Asset::Count) t.tail.ch('-').str(asset_name(q));
        // The liquidation order bought: a SHORT position was closed.
        t.tail.str((w.flags & kEvBuy) ? "  SHORT  " : "  LONG  ");
        usd_short(t.tail, w.usd_value);
      } else {
        t.marker.str("WHALE T").u64(w.tier);
        t.tail.str("  ");
        usd_short(t.tail, w.usd_value);
        if (w.kind == EventKind::Trade) {
          t.tail.str("  ").str(asset_name(w.asset));
          if (q != Asset::Count) t.tail.ch('-').str(asset_name(q));
          t.tail.str((w.flags & kEvBuy) ? "  BUY" : "  SELL");
        } else {
          const bool hex = w.feed == FeedId::Eth;
          t.tail.str("  ");
          tag(t.tail, w.from_tag, hex);
          t.tail.str(" ").ch(kGlyphArrow).str(" ");
          tag(t.tail, w.to_tag, hex);
        }
      }
      if (w.label != SourceLabel::Live) t.tail.str("  ").str(label_name(w.label));
      t.text.clear();
      t.text.str(t.head.view()).str(t.marker.view()).str(t.tail.view());
      t.hit.valid = true;
      t.hit.hub = liq || w.kind == EventKind::Trade ? w.dst_hub : w.src_hub;
      t.hit.id_hash = w.id_hash;
      for (int k = 0; k < 4; ++k) {
        t.hit.from_tag[k] = w.from_tag[k];
        t.hit.to_tag[k] = w.to_tag[k];
      }
    }
    // 1 Hz scroll smoothing: a new row slides the list down over one second.
    if (tape_lines_ > 0 && newest != prevNewest && prev > 0) scroll_start_s_ = in.now_s;
    tape_newest_ = newest;
    tape_header_.clear();
    tape_header_.str("WHALE ").ch('&').str(" LIQUIDATION TAPE");
    for (std::size_t k = 0; k < chips_.size(); ++k) {
      chips_[k].clear();
      if (!in.filter || !in.filter->active(static_cast<FilterKind>(k))) continue;
      char buf[24];
      in.filter->chip(static_cast<FilterKind>(k), buf, sizeof buf);
      chips_[k].str(buf).str(" x");
    }
  }

  // ---- trading sessions: "LONDON    OPEN    02:14 to close" ---------------------------
  session_lines_ = 0;
  if (in.sessions) {
    const std::int64_t now = snap.now_ms;
    for (std::size_t e = 0; e < in.sessions->size() && session_lines_ < sessions_.size(); ++e) {
      const SessionStatus st = in.sessions->state(e, now);
      char buf[64];
      format_session_chip(in.sessions->exchange(e), st, now, buf, sizeof buf);
      sessions_[session_lines_].clear();
      sessions_[session_lines_].str(buf);
      session_state_[session_lines_] = static_cast<std::uint8_t>(st.state);
      ++session_lines_;
    }
  }

  // ---- sun -------------------------------------------------------------------------
  sun_line_.clear();
  if (in.sun_ms != 0) {
    sun_line_.str("SUN ");
    hhmmss(sun_line_, in.sun_ms);
    sun_line_.str(in.sun_override ? " UTC (FIXED)  SUBSOLAR " : " UTC  SUBSOLAR ");
    sun_line_.fixed(std::fabs(in.subsolar_lat), 1).ch(in.subsolar_lat >= 0.0 ? 'N' : 'S').ch(' ');
    sun_line_.fixed(std::fabs(in.subsolar_lon), 1).ch(in.subsolar_lon >= 0.0 ? 'E' : 'W');
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
    if (session_lines_ > 0) {
      y += lh * 0.8f;
      for (std::size_t i = 0; i < session_lines_; ++i) {
        const auto st = static_cast<SessionState>(session_state_[i]);
        hud.text(l.feeds_x, y, s, st == SessionState::Open ? kInk : st == SessionState::Closed || st == SessionState::Holiday ? kInkDim : kInkMid,
                 sessions_[i].view());
        y += lh;
      }
    }
    if (sun_line_.size() > 0) {
      y += lh * 0.8f;
      hud.text(l.feeds_x, y, s, kInkMid, sun_line_.view());
      y += lh;
    }
    if (in.routes_drawn) {
      hud.text(l.feeds_x, y, s, kInkDim, "ARCS  ROUTE \x84 MODELED (cable geometry)");
      y += lh;
    }
  }

  // Whale & liquidation tape: right-hand column, newest on top. Text in Ink
  // (older rows dimmer); only the tier / LIQ marker is Neon Gold.
  {
    const float right = l.tape_right;
    float y = l.tape_y;
    const float headerW = static_cast<float>(tape_header_.size()) * adv;
    hud.text(right - headerW, y, s, kInkDim, tape_header_.view());
    y += lh;
    // Filter chips, right-aligned: ">10M x  VENUE OKX x".
    float cx = right;
    for (std::size_t k = chips_.size(); k-- > 0;) {
      if (chips_[k].size() == 0) continue;
      const float w = static_cast<float>(chips_[k].size()) * adv;
      cx -= w;
      hud.text(cx, y, s, kAmber, chips_[k].view());
      hud.panel(cx, y + lh - 2.0f * s, w - 2.0f * adv, 1.0f, kAmberDim);
      cx -= 2.0f * adv;
    }
    y += lh;
    // Scroll smoothing: the list eases down by one row over 1 s after a new row.
    const double ease = std::clamp(in.now_s - scroll_start_s_, 0.0, 1.0);
    const float slide = static_cast<float>((1.0 - (1.0 - ease) * (1.0 - ease)));
    const float offset = (slide - 1.0f) * l.tape_row_h;
    std::size_t widest = 0;
    for (std::size_t i = 0; i < tape_lines_; ++i) widest = std::max(widest, tape_[i].text.size());
    const float x0 = right - static_cast<float>(widest) * adv;
    for (std::size_t i = 0; i < tape_lines_; ++i) {
      const TapeLine& t = tape_[i];
      const float yy = y + static_cast<float>(i) * l.tape_row_h + (i == 0 ? 0.0f : offset);
      const std::uint32_t c = i == 0 ? kInk : i < 4 ? kInkMid : kInkDim;
      float x = hud.text(x0, yy, s, c, t.head.view());
      x = hud.text(x, yy, s, kNeonGold, t.marker.view());
      hud.text(x, yy, s, c, t.tail.view());
    }
    if (tape_lines_ == 0) hud.text(right - 18.0f * adv, y, s, kInkDim, "NO WHALES YET");
  }

  // Command bar: one monospace line, no box, Amber underline and caret.
  if (in.bar && in.bar->is_open()) {
    const CommandBar& b = *in.bar;
    const float x = l.bar_x, y = l.bar_y;
    float cx = hud.text(x, y, s, kAmber, "/ ");
    cx = hud.text(cx, y, s, kInk, b.text());
    const bool caretOn = std::fmod(in.now_s, 1.0) < 0.6;
    if (caretOn) hud.panel(cx + 1.0f * s, y - 1.0f * s, 1.0f * s, lh, kAmber);
    hud.panel(x, y + lh + 1.0f * s, l.bar_w, 1.0f, kAmber);
    float ry = y + lh * 1.8f;
    for (std::size_t i = 0; i < b.result_count(); ++i) {
      const bool sel = i == b.selection();
      if (sel) hud.text(x - 2.0f * adv, ry, s, kAmber, ">");
      hud.text(x, ry, s, sel ? kInk : kInkMid, b.result(i).label);
      ry += lh;
    }
    if (b.result_count() == 0 && !b.text().empty()) hud.text(x, ry, s, kInkDim, "NO MATCH");
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

TapeHit LiveHud::tape_hit(const LiveHudLayout& l, double x, double y) const noexcept {
  const float s = l.scale;
  const float lh = HudRenderer::line_height(s);
  const float top = l.tape_y + 2.0f * lh;
  std::size_t widest = 0;
  for (std::size_t i = 0; i < tape_lines_; ++i) widest = std::max(widest, tape_[i].text.size());
  const float x0 = l.tape_right - static_cast<float>(widest) * 6.0f * s;
  if (x < x0 || x > l.tape_right || y < top) return {};
  const auto row = static_cast<std::size_t>((y - top) / l.tape_row_h);
  if (row >= tape_lines_) return {};
  return tape_[row].hit;
}

TapeHit LiveHud::find_on_tape(std::string_view q, bool tx) const noexcept {
  if (q.empty()) return {};
  if (tx) {
    const std::uint64_t h = source_id_hash(q);
    for (std::size_t i = 0; i < tape_lines_; ++i) {
      if (tape_[i].hit.valid && tape_[i].hit.id_hash == h) return tape_[i].hit;
    }
    return {};
  }
  const bool evm = q.size() > 2 && q[0] == '0' && (q[1] == 'x' || q[1] == 'X');
  if (evm) q.remove_prefix(2);
  if (q.size() < 4) return {};
  const char tag[4] = {q[0], q[1], q[q.size() - 2], q[q.size() - 1]};
  auto same = [evm](const char* a, const char* b) {
    for (int k = 0; k < 4; ++k) {
      const char x = evm ? static_cast<char>(std::tolower(static_cast<unsigned char>(a[k]))) : a[k];
      const char y = evm ? static_cast<char>(std::tolower(static_cast<unsigned char>(b[k]))) : b[k];
      if (x != y) return false;
    }
    return true;
  };
  for (std::size_t i = 0; i < tape_lines_; ++i) {
    const TapeHit& h = tape_[i].hit;
    if (h.valid && (same(h.from_tag, tag) || same(h.to_tag, tag))) return h;
  }
  return {};
}

FilterKind LiveHud::chip_hit(const LiveHudLayout& l, double x, double y) const noexcept {
  const float s = l.scale;
  const float lh = HudRenderer::line_height(s);
  const float adv = 6.0f * s;
  const float cy = l.tape_y + lh;
  if (y < cy || y > cy + lh) return FilterKind::Count;
  float cx = l.tape_right;
  for (std::size_t k = chips_.size(); k-- > 0;) {
    if (chips_[k].size() == 0) continue;
    const float w = static_cast<float>(chips_[k].size()) * adv;
    cx -= w;
    if (x >= cx && x <= cx + w) return static_cast<FilterKind>(k);
    cx -= 2.0f * adv;
  }
  return FilterKind::Count;
}

}  // namespace ftv
