#pragma once
// HUD content: frame timing, per-pass GPU timings (GL_TIME_ELAPSED), draw and
// edge counts, layout state, ingest statistics, target checks, the hub legend,
// selection details and key help.
//
// Text is formatted into fixed-size buffers (FixedText) a few times per
// second and re-emitted as glyph instances every frame. After construction
// nothing in this class allocates, so it is safe inside the render loop.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/fixed_text.hpp"
#include "ingest/ingest.hpp"
#include "layout/layout_engine.hpp"
#include "model/graph.hpp"
#include "model/hub_taxonomy.hpp"
#include "render/hud.hpp"
#include "render/renderer.hpp"

namespace ftv {

struct FrameStats {
  double fps = 0.0;
  double frame_ms = 0.0;      // smoothed wall time between frames
  double cpu_ms = 0.0;        // smoothed render-thread CPU per frame (excludes fence wait and swap)
  double cpu_peak_ms = 0.0;   // worst frame since the last refresh
  std::uint64_t allocs_last_frame = 0;
  std::uint64_t allocs_total = 0;
  bool alloc_counting = false;
  bool inter_detail = true;
  bool color_by_channel = false;
  bool hud_visible = true;
};

constexpr std::uint32_t pack_rgba(unsigned r, unsigned g, unsigned b, unsigned a) noexcept {
  return (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16) | ((a & 255u) << 24);
}

class HudStats {
 public:
  HudStats(const Graph& graph, const HubTaxonomy& taxonomy, const IngestResult& ingest, std::string_view inputName);

  // Rebuilds all text lines from the current counters.
  void refresh(const FrameStats& frame, const Renderer& renderer, const LayoutStatus& layout,
               std::optional<std::uint32_t> selection) noexcept;
  // Writes glyph instances for the current lines into the HUD ring.
  void emit(HudRenderer& hud, int width, int height) const noexcept;

  static float text_scale(int height) noexcept;

 private:
  static constexpr std::size_t kSegLen = 112;
  static constexpr std::size_t kSegsPerLine = 8;
  static constexpr std::size_t kMaxLines = 20;
  static constexpr std::size_t kMaxLegend = 32;

  struct Seg {
    FixedText<kSegLen> text;
    std::uint32_t color = 0;
  };
  struct Line {
    std::array<Seg, kSegsPerLine> segs;
    std::size_t count = 0;
    void clear() noexcept { count = 0; }
    FixedText<kSegLen>& add(std::uint32_t color) noexcept;
    [[nodiscard]] std::size_t chars() const noexcept;
  };
  struct Block {
    std::array<Line, kMaxLines> lines;
    std::size_t count = 0;
    void clear() noexcept { count = 0; }
    Line& next() noexcept;
  };

  void emit_block(HudRenderer& hud, const Block& b, float x, float y, float scale, bool panelBg) const noexcept;

  const Graph& graph_;
  const HubTaxonomy& taxonomy_;
  const IngestResult& ingest_;

  Block stats_;     // top-left
  Block select_;    // below the stats block
  Block help_;      // bottom-left
  Block legend_;    // top-right
  std::array<std::uint32_t, kMaxLegend> legend_colors_{};
  std::size_t legend_count_ = 0;

  // Static lines prepared at construction (dataset facts never change).
  FixedText<kSegLen> title_;
  FixedText<kSegLen> ingest_line1_;
  FixedText<kSegLen> ingest_line2_;
  FixedText<kSegLen> graph_line_;
};

}  // namespace ftv
