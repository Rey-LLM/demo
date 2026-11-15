#include "app/hud_stats.hpp"

#include <algorithm>
#include <cmath>

namespace ftv {

namespace {

// The HUD uses the theme's hues only: ink (#D9DCE1) at three alphas, amber
// (#C8962E) for anything that misses a target, obsidian (#050608) panels.
constexpr std::uint32_t kWhite = pack_rgba(0xD9, 0xDC, 0xE1, 178);
constexpr std::uint32_t kDim = pack_rgba(0xD9, 0xDC, 0xE1, 110);
constexpr std::uint32_t kAccent = pack_rgba(0xD9, 0xDC, 0xE1, 220);
constexpr std::uint32_t kGood = kWhite;
constexpr std::uint32_t kBad = pack_rgba(0xC8, 0x96, 0x2E, 230);
constexpr std::uint32_t kWarn = kBad;
constexpr std::uint32_t kPanel = pack_rgba(0x05, 0x06, 0x08, 200);

}  // namespace

FixedText<HudStats::kSegLen>& HudStats::Line::add(std::uint32_t color) noexcept {
  // When full, keep appending to the last segment (text is truncated there).
  if (count < kSegsPerLine) {
    segs[count].text.clear();
    segs[count].color = color;
    ++count;
  }
  return segs[count - 1].text;
}

std::size_t HudStats::Line::chars() const noexcept {
  std::size_t n = 0;
  for (std::size_t i = 0; i < count; ++i) n += segs[i].text.size();
  return n;
}

HudStats::Line& HudStats::Block::next() noexcept {
  if (count < kMaxLines) {
    lines[count].clear();
    ++count;
  }
  return lines[count - 1];
}

HudStats::HudStats(const Graph& graph, const HubTaxonomy& taxonomy, const IngestResult& ingest,
                   std::string_view inputName)
    : graph_(graph), taxonomy_(taxonomy), ingest_(ingest) {
  const IngestStats& st = ingest.stats;
  title_.str("FTVIZ  ").str(inputName);
  ingest_line1_.str("INGEST ")
      .grouped(st.rows_ok)
      .str(" ROWS  ")
      .si(st.rows_per_sec(), 2)
      .str(" ROWS/S  ")
      .fixed(st.mb_per_sec(), 0)
      .str(" MB/S  ")
      .u64(st.parser_threads)
      .str(" PARSERS");
  ingest_line2_.str("MALFORMED ")
      .grouped(st.rows_malformed)
      .str("  ENTITIES ")
      .grouped(st.unique_entities)
      .str("  AGG EDGES ")
      .grouped(st.aggregated_edges)
      .str("  WALL ")
      .fixed(st.wall_ms / 1000.0, 2)
      .str(" S");
  graph_line_.str("GRAPH ")
      .grouped(graph.node_count())
      .str(" NODES  ")
      .grouped(graph.edge_count())
      .str(" EDGES  ")
      .grouped(graph.inter_hub_edges)
      .str(" INTER-HUB  ")
      .grouped(graph.bridge_count)
      .str(" BRIDGES");

  // Hub legend (static).
  legend_.clear();
  {
    Line& head = legend_.next();
    head.add(kAccent).str("KEY HUB").pad_to(22).str("NODES");
  }
  legend_count_ = std::min<std::size_t>(taxonomy.size(), std::min(kMaxLegend, kMaxLines - 1));
  std::size_t nameWidth = 0;
  for (std::size_t h = 0; h < legend_count_; ++h) {
    const HubDef& d = taxonomy.hubs()[h];
    nameWidth = std::max(nameWidth, (d.display_name.empty() ? d.id : d.display_name).size());
  }
  nameWidth = std::min<std::size_t>(nameWidth + 2, kSegLen - 1);
  legend_.lines[0].segs[0].text.clear();
  legend_.lines[0].segs[0].text.str("KEY HUB").pad_to(4 + nameWidth).str("NODES");
  for (std::size_t h = 0; h < legend_count_; ++h) {
    const HubDef& d = taxonomy.hubs()[h];
    Line& l = legend_.next();
    auto& key = l.add(kWhite);
    if (h < 9) {
      key.u64(h + 1).str("   ");
    } else {
      key.str("    ");
    }
    auto& name = l.add(kWhite);
    name.str(d.display_name.empty() ? std::string_view(d.id) : std::string_view(d.display_name));
    name.pad_to(nameWidth);
    const std::uint32_t pop = h < graph.hub_population.size() ? graph.hub_population[h] : 0u;
    l.add(kDim).grouped(pop);
  }
}

float HudStats::text_scale(int height) noexcept {
  // 1.0 at 720p, 1.5 at 1080p, 2.0 at 1440p (in steps of 0.5).
  const float s = std::round(static_cast<float>(height) / 720.0f * 2.0f) * 0.5f;
  return std::clamp(s, 1.0f, 3.0f);
}

void HudStats::refresh(const FrameStats& f, const Renderer& r, const LayoutStatus& layout,
                       std::optional<std::uint32_t> selection) noexcept {
  const GpuTimers& t = r.timers();
  const RenderCounters& c = r.counters();

  // ---- stats block ---------------------------------------------------------
  stats_.clear();
  stats_.next().add(kAccent).str(title_.view());
  {
    Line& l = stats_.next();
    l.add(kDim).str("FPS ");
    l.add(f.fps >= 60.0 ? kGood : kBad).fixed(f.fps, 1);
    l.add(kDim).str("  FRAME ");
    l.add(kWhite).fixed(f.frame_ms, 2).str(" MS");
    l.add(kDim).str("  CPU ");
    l.add(f.cpu_ms <= 2.0 ? kGood : kBad).fixed(f.cpu_ms, 2).str(" MS");
    l.add(kDim).str(" (PEAK ").fixed(f.cpu_peak_ms, 2).str(", UPLOAD ").fixed(c.upload_ms, 2).str(")");
  }
  {
    Line& l = stats_.next();
    l.add(kDim).str("GPU ");
    l.add(t.total_ms() <= 16.6f ? kGood : kBad).fixed(t.total_ms(), 2).str(" MS");
    auto& d = l.add(kDim);
    for (std::size_t p = 0; p < kGpuPassCount; ++p) {
      d.str("  ").str(kGpuPassNames[p]).ch(' ').fixed(t.ms(static_cast<GpuPass>(p)), 2);
    }
  }
  {
    Line& l = stats_.next();
    l.add(kDim).str("DRAWS SCENE ");
    l.add(c.scene_draws <= 8 ? kWhite : kBad).u64(c.scene_draws);
    l.add(kDim).str(" + POST ").u64(c.post_draws).str("  ARCS ");
    l.add(kWhite).grouped(static_cast<std::uint64_t>(c.visible_intra) + c.visible_inter);
    l.add(kDim)
        .str(" / CAP ")
        .grouped(r.visible_capacity())
        .str("  (INTRA ")
        .grouped(c.visible_intra)
        .str(", INTER ")
        .grouped(c.visible_inter)
        .str(")");
  }
  {
    Line& l = stats_.next();
    l.add(kDim).str("EDGES INTRA ").grouped(r.intra_edge_count()).str(" (BUDGET ").grouped(r.intra_budget());
    l.add(kDim).str(")  INTER ").grouped(r.inter_edge_count()).str(" (BUDGET ").grouped(r.inter_budget()).str(")");
    l.add(kDim).str("  WHALES ").grouped(r.whale_count()).str(" (ACTIVE ").u64(c.active_whales).str(")");
  }
  {
    Line& l = stats_.next();
    l.add(kDim).str("LAYOUT ");
    const char* state = !layout.running ? "FROZEN"
                        : layout.paused ? "PAUSED"
                                        : (layout.converged ? "CONVERGED" : "RUNNING");
    l.add(!layout.running || layout.paused ? kWarn : (layout.converged ? kGood : kAccent)).str(state);
    l.add(kDim).str("  ITER ").grouped(layout.iteration).str("  ");
    l.add(layout.iteration_ms <= 50.0f ? kWhite : kWarn).fixed(layout.iteration_ms, 1).str(" MS");
    l.add(kDim)
        .str(" (TREE ")
        .fixed(layout.tree_ms, 1)
        .str(" REP ")
        .fixed(layout.repulsion_ms, 1)
        .str(" INT ")
        .fixed(layout.integrate_ms, 1)
        .str(")  T ")
        .fixed(layout.temperature, 2)
        .str("  DISP ")
        .fixed(layout.mean_displacement, 3)
        .str("  SNAP ")
        .grouped(c.snapshot_iteration);
  }
  stats_.next().add(kDim).str(graph_line_.view());
  stats_.next().add(kDim).str(ingest_line1_.view());
  stats_.next().add(kDim).str(ingest_line2_.view());
  {
    Line& l = stats_.next();
    l.add(kDim).str("TARGETS ");
    const bool fpsOk = f.fps >= 60.0;
    const bool cpuOk = f.cpu_ms <= 2.0;
    const bool gpuOk = t.total_ms() <= 16.6f;
    const bool drawOk = c.scene_draws <= 8;
    l.add(fpsOk ? kGood : kBad).str(fpsOk ? "FPS>=60 OK  " : "FPS>=60 MISS  ");
    l.add(cpuOk ? kGood : kBad).str(cpuOk ? "CPU<=2MS OK  " : "CPU<=2MS MISS  ");
    l.add(gpuOk ? kGood : kBad).str(gpuOk ? "GPU<=16.6MS OK  " : "GPU<=16.6MS MISS  ");
    l.add(drawOk ? kGood : kBad).str(drawOk ? "SCENE DRAWS<=8 OK  " : "SCENE DRAWS<=8 MISS  ");
    if (f.alloc_counting) {
      const bool allocOk = f.allocs_last_frame == 0;
      auto& a = l.add(allocOk ? kGood : kBad);
      a.str("ALLOC/FRAME ").u64(f.allocs_last_frame).str(allocOk ? " OK  " : " MISS  ");
    } else {
      l.add(kDim).str("ALLOC CHECK: DEBUG BUILDS  ");
    }
    if (layout.iteration_ms <= 0.0f) {
      l.add(kDim).str("LAYOUT<=50MS N/A");  // no iteration measured yet
    } else if (!layout.converged && !layout.paused) {
      const bool layOk = layout.iteration_ms <= 50.0f;
      l.add(layOk ? kGood : kWarn).str(layOk ? "LAYOUT<=50MS OK" : "LAYOUT<=50MS MISS");
    }
  }

  // ---- selection -------------------------------------------------------------
  select_.clear();
  if (selection && *selection < graph_.node_count()) {
    const std::uint32_t n = *selection;
    const std::uint16_t hub = graph_.nodes.hub[n];
    const std::uint16_t flags = graph_.nodes.flags[n];
    {
      Line& l = select_.next();
      l.add(kAccent).str("SELECTED #").u64(n).str("  ");
      l.add(kWhite).str(ingest_.label(n));
    }
    {
      Line& l = select_.next();
      l.add(kDim).str("HUB ");
      if (hub < taxonomy_.size()) {
        const HubDef& d = taxonomy_.hubs()[hub];
        l.add(kWhite).str(d.display_name.empty() ? std::string_view(d.id) : std::string_view(d.display_name));
      }
      l.add(kDim)
          .str("  DEGREE ")
          .grouped(graph_.nodes.degree[n])
          .str("  TX ")
          .grouped(static_cast<std::uint64_t>(graph_.nodes.weighted_degree[n]));
      if (flags & kNodeBridge) l.add(kWarn).str("  BRIDGE");
      if (flags & kNodeUnresolved) l.add(kWarn).str("  UNRESOLVED");
    }
    {
      const glm::vec3 p = r.node_world_position(n);
      Line& l = select_.next();
      l.add(kDim).str("POS ").fixed(p.x, 1).str(", ").fixed(p.y, 1).str(", ").fixed(p.z, 1);
      l.add(kDim).str("   [F] FRAME  [ESC] CLEAR");
    }
  }

  // ---- key help -------------------------------------------------------------
  help_.clear();
  help_.next().add(kDim).str(
      "[LMB DRAG] ORBIT  [RMB/MMB DRAG] PAN  [WHEEL] ZOOM  [CLICK] PIN HUB / SELECT NODE  [1-5] VIEWS  [E] EXPORT");
  {
    Line& l = help_.next();
    l.add(kDim).str("[F] FRAME  [D] INTER-HUB EDGES: ");
    l.add(f.inter_detail ? kGood : kWarn).str(f.inter_detail ? "ON" : "OFF");
    l.add(kDim).str("  [L] LAYOUT: ");
    l.add(!layout.running || layout.paused ? kWarn : kGood)
        .str(!layout.running ? "FROZEN" : (layout.paused ? "PAUSED" : "ON"));
    l.add(kDim).str("  [R] REHEAT  [SPACE] ");
    l.add(f.replay_paused ? kWarn : kGood).str(f.replay_paused ? "PAUSED" : "RUNNING");
    l.add(kDim).str("  [</>] SEEK  T ").fixed(f.replay_time, 1).str(" S  [H] HUD  [ESC] CLEAR/QUIT");
  }
}

void HudStats::emit_block(HudRenderer& hud, const Block& b, float x, float y, float scale, bool panelBg) const noexcept {
  if (b.count == 0) return;
  const float lh = HudRenderer::line_height(scale);
  const float adv = 6.0f * scale;
  if (panelBg) {
    std::size_t maxChars = 0;
    for (std::size_t i = 0; i < b.count; ++i) maxChars = std::max(maxChars, b.lines[i].chars());
    hud.panel(x - 6.0f * scale, y - 4.0f * scale, static_cast<float>(maxChars) * adv + 12.0f * scale,
              static_cast<float>(b.count) * lh + 6.0f * scale, kPanel);
  }
  for (std::size_t i = 0; i < b.count; ++i) {
    float cx = x;
    const Line& l = b.lines[i];
    for (std::size_t s = 0; s < l.count; ++s) cx = hud.text(cx, y, scale, l.segs[s].color, l.segs[s].text.view());
    y += lh;
  }
}

void HudStats::emit(HudRenderer& hud, int width, int height) const noexcept {
  const float s = text_scale(height);
  const float lh = HudRenderer::line_height(s);
  const float margin = 10.0f * s;

  emit_block(hud, stats_, margin, margin, s, true);
  const float selY = margin + static_cast<float>(stats_.count + 1) * lh;
  emit_block(hud, select_, margin, selY, s, true);
  emit_block(hud, help_, margin, static_cast<float>(height) - margin - static_cast<float>(help_.count) * lh, s, true);

  // Hub legend in the bottom-right corner (the top-right belongs to EXPORT
  // and the hub panel).
  std::size_t maxChars = 0;
  for (std::size_t i = 0; i < legend_.count; ++i) maxChars = std::max(maxChars, legend_.lines[i].chars());
  const float adv = 6.0f * s;
  const float lx = static_cast<float>(width) - margin - static_cast<float>(maxChars) * adv;
  emit_block(hud, legend_, lx, static_cast<float>(height) - margin - static_cast<float>(legend_.count) * lh, s, true);
}

float HudStats::left_bottom(int height) const noexcept {
  const float s = text_scale(height);
  const float lh = HudRenderer::line_height(s);
  return 10.0f * s + static_cast<float>(stats_.count + 1 + (select_.count ? select_.count + 1 : 0)) * lh;
}

}  // namespace ftv
