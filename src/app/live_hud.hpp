#pragma once
// HUD elements of the live layer: a summary line (live streams, total
// transactions per second), one status row per feed (state, rate, latency),
// the whale tape, the EXPORT text button and its toast, the pinned hub's
// panel and the replay timeline. Monochrome Ink at three alphas plus Amber
// for the timeline and tier-3 whales; no boxes.
//
// refresh() formats text into fixed buffers at 4 Hz from the latest model
// snapshot; emit() writes glyphs every frame. Neither allocates.

#include <array>
#include <cstdint>
#include <string_view>

#include "core/fixed_text.hpp"
#include "ingest/live/live_runtime.hpp"
#include "model/hub_taxonomy.hpp"
#include "render/hud.hpp"

namespace ftv {

struct HudRect {
  float x = 0, y = 0, w = 0, h = 0;
  [[nodiscard]] bool contains(double px, double py) const noexcept {
    return px >= x && px <= x + w && py >= y && py <= y + h;
  }
};

struct LiveHudLayout {
  float scale = 1.0f;        // glyph scale of the feed rows, button and timeline text
  float panel_scale = 1.375f;  // hub panel: 11 px glyph cells
  HudRect export_button;
  HudRect timeline;          // hit area around the 1 px line
  float timeline_y = 0;      // y of the line
  float feeds_x = 0, feeds_y = 0;
  float panel_right = 0, panel_y = 0;
  float toast_y = 0;
};

struct LiveHudInput {
  int pinned_hub = -1;
  bool export_hover = false;
  bool timeline_visible = false;
  double timeline_pos_s = 0.0, timeline_len_s = 0.0;
  bool capture_synthetic = false;
  bool paused = false;
  bool show_feeds = true;
  std::string_view toast;  // empty: none
  bool toast_ok = true;
};

class LiveHud {
 public:
  LiveHud(const HubTaxonomy& taxonomy, ModelMode mode, const LiveFeeds* feeds, const FeedsConfig* config)
      : taxonomy_(taxonomy), mode_(mode), feeds_(feeds), config_(config) {}

  // `feedsTop` is the y (px) below the statistics block on the left.
  static LiveHudLayout layout(int width, int height, float feedsTop) noexcept;

  void refresh(const LiveSnapshot& snap, const LiveHudInput& in) noexcept;
  void emit(HudRenderer& hud, const LiveHudLayout& l, const LiveHudInput& in) const noexcept;

  // Text of feed row `f` (tests / console report).
  [[nodiscard]] std::string_view feed_row(std::size_t f) const noexcept { return rows_[f].text.view(); }
  [[nodiscard]] std::string_view summary() const noexcept { return summary_.view(); }
  [[nodiscard]] std::string_view tape_line(std::size_t i) const noexcept { return tape_[i].text.view(); }
  [[nodiscard]] std::size_t tape_lines() const noexcept { return tape_lines_; }

 private:
  struct Row {
    FixedText<96> text;
    std::uint8_t dot = 0;  // 0 hidden, 1 Ink, 2 Ink at 30%, 3 ring (REPLAY)
  };
  const HubTaxonomy& taxonomy_;
  ModelMode mode_;
  const LiveFeeds* feeds_;
  const FeedsConfig* config_;
  struct TapeLine {
    FixedText<96> text;
    std::uint8_t tier = 1;
  };
  std::array<Row, kFeedCount> rows_{};
  FixedText<96> summary_;
  std::array<TapeLine, LiveSnapshot::kTapeLength> tape_{};
  std::size_t tape_lines_ = 0;
  std::array<FixedText<96>, 12> panel_{};
  std::size_t panel_lines_ = 0;
  FixedText<96> timeline_text_;
};

}  // namespace ftv
