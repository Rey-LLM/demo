#pragma once
// The instanced GPU renderer. Owns every GL object and records a fixed,
// small number of draw calls per frame regardless of graph size:
//
//   compute: edge cull (+ clamp)                  (dispatches, not draws)
//   Pass 4  nodes        glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)
//   Pass 1  hub clouds   glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, hubs*P)
//   Pass 2  corridors    glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4*SEG, B)
//   Pass 3  edges        glDrawArraysIndirect x2 (intra LINES, inter LINE_STRIP)
//   Pass 5  picking      glDrawArraysInstanced (only on click)
//   Pass 6  HUD          glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, glyphs)
// => at most 7 draw calls. Passes keep the spec's numbering; opaque nodes are
// executed first so the translucent passes depth-test against them.
//
// Per-frame CPU work: one fence wait, optional memcpy of a new layout
// snapshot into a free region of the persistently mapped position ring, a
// ~400-byte uniform write, HUD glyph writes, and the draw calls. No buffer
// (re)specification, no glBufferData/SubData, no heap allocation.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/triple_buffer.hpp"
#include "layout/layout_engine.hpp"
#include "model/corridors.hpp"
#include "model/graph.hpp"
#include "model/hub_taxonomy.hpp"
#include "render/gl_context.hpp"
#include "render/gl_raii.hpp"
#include "render/gpu_timers.hpp"
#include "render/hud.hpp"
#include "render/picking.hpp"
#include "render/render_settings.hpp"

namespace ftv {

struct FrameParams {
  glm::mat4 view{1.0f};
  glm::mat4 proj{1.0f};
  glm::vec3 camera_pos{0.0f};
  float time = 0.0f;
  float near_plane = 0.5f;
  int width = 0;
  int height = 0;
  bool inter_detail = false;  // E key
  bool color_by_channel = false;
  bool show_hud = true;
};

struct RenderCounters {
  std::uint32_t visible_intra = 0;
  std::uint32_t visible_inter = 0;
  std::uint32_t draw_calls = 0;
  std::uint64_t snapshot_iteration = 0;
  std::uint64_t snapshots_uploaded = 0;
  float fence_wait_ms = 0.0f;  // CPU time blocked on the frame-slot fence (GPU-bound time)
  float upload_ms = 0.0f;      // CPU time copying a new layout snapshot into the ring (0 when none)
};

class Renderer {
 public:
  static constexpr unsigned kSlots = 3;
  static constexpr int kCorridorSegments = 16;
  static constexpr std::uint32_t kMaxVisibleEdges = 2000000;  // hard cap on drawn edges per frame
  // Most bytes of a layout snapshot copied into the position ring per frame.
  // A 1M-node snapshot (12 MB) is spread over 3 frames, so no single frame
  // pays for the whole copy (it measured ~2 ms in one go on the test VM).
  static constexpr std::size_t kUploadBytesPerFrame = std::size_t{4} << 20;

  Renderer(const Graph& graph, const HubTaxonomy& taxonomy, const std::vector<CorridorBundle>& bundles,
           const RenderSettings& settings, const GlCaps& caps, std::string shaderDir);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool init(std::string& error);

  // Frame protocol: begin_frame() waits for the slot fence and consumes the
  // newest layout snapshot; the caller then writes HUD text via hud(); then
  // draw_frame() records all passes and fences the slot.
  void begin_frame(TripleBuffer<PositionSnapshot>& snapshots);
  // Copies `snapshot` into every position region (used right after init(),
  // e.g. when the renderer is re-created after a context loss while the
  // layout is idle and will not publish again).
  void prime_positions(const PositionSnapshot& snapshot);
  HudRenderer& hud() noexcept { return hud_; }
  void draw_frame(const FrameParams& params);

  // Selection highlight (1-hop neighbourhood) as bits in a small SSBO.
  void set_selection(std::optional<std::uint32_t> node);
  [[nodiscard]] std::optional<std::uint32_t> selection() const noexcept { return selected_; }

  void request_pick(int x, int y) noexcept { picker_.request(x, y); }
  bool poll_pick(std::optional<std::uint32_t>& node) noexcept { return picker_.poll(node); }

  // Latest uploaded node position (from the CPU copy of the snapshot).
  [[nodiscard]] glm::vec3 node_position(std::uint32_t node) const noexcept;

  [[nodiscard]] const GpuTimers& timers() const noexcept { return timers_; }
  [[nodiscard]] const RenderCounters& counters() const noexcept { return counters_; }
  [[nodiscard]] std::uint32_t intra_edge_count() const noexcept { return intra_count_; }
  [[nodiscard]] std::uint32_t inter_edge_count() const noexcept { return inter_count_; }
  [[nodiscard]] std::uint32_t intra_budget() const noexcept { return intra_lod_; }
  [[nodiscard]] std::uint32_t inter_budget() const noexcept { return inter_lod_; }
  [[nodiscard]] std::uint32_t visible_capacity() const noexcept { return intra_cap_ + inter_cap_; }
  [[nodiscard]] bool gpu_culling() const noexcept { return settings_.gpu_culling; }
  [[nodiscard]] std::size_t bundle_count() const noexcept { return bundle_count_; }

  // Reads back the default framebuffer (for --screenshot). Startup/test only.
  bool save_screenshot(const std::string& path, int width, int height) const;

 private:
  struct FrameUniforms {  // std140, mirrors `Frame` in common.glsl
    glm::mat4 view, proj, view_proj;
    glm::vec4 camera;
    glm::vec4 viewport;
    glm::vec4 params0;
    glm::vec4 params1;
    glm::uvec4 counts;
  };

  bool create_programs(std::string& error);
  void upload_static_data();
  void build_palette();
  void ensure_targets(int width, int height);
  void refresh_highlight_region(unsigned slot) noexcept;

  const Graph& graph_;
  const HubTaxonomy& taxonomy_;
  const std::vector<CorridorBundle>& bundles_;
  RenderSettings settings_;
  GlCaps caps_;
  std::string shader_dir_;

  // Programs.
  gl::Program prog_node_, prog_pick_, prog_hub_, prog_bundle_, prog_edge_, prog_hud_, prog_cull_, prog_clamp_;
  GLint loc_edge_inter_ = -1, loc_edge_direct_ = -1, loc_edge_first_ = -1, loc_bundle_segments_ = -1;
  GLint loc_cull_[7] = {-1, -1, -1, -1, -1, -1, -1};
  GLint loc_clamp_[2] = {-1, -1};

  // Vertex arrays.
  gl::VertexArray vao_empty_;
  gl::VertexArray vao_nodes_;

  // Immutable data.
  gl::Buffer node_static_;
  gl::Buffer edges_;
  gl::Buffer hubs_;
  gl::Buffer bundles_buf_;
  gl::Buffer visible_intra_;
  gl::Buffer visible_inter_;
  gl::Buffer indirect_;
  gl::Texture palette_;

  // Persistently mapped rings.
  gl::PersistentBuffer positions_;  // 3 regions of N*12 bytes
  gl::PersistentBuffer uniforms_;   // 3 regions
  gl::PersistentBuffer highlight_;  // 3 regions of ceil(N/32)*4 bytes
  gl::PersistentBuffer readback_;   // 3 x indirect-command copies (visible counts)
  GLsizeiptr pos_region_bytes_ = 0, ubo_region_bytes_ = 0, hl_region_bytes_ = 0;
  std::array<unsigned, kSlots> pos_region_of_slot_{};
  unsigned pos_region_ = 0;
  // Amortised snapshot upload: region being filled from `fill_source_` over
  // several frames. It is not drawn from until complete, and the source (the
  // triple buffer's reader-owned front slot) stays valid because no new
  // snapshot is acquired while a fill is in progress.
  int fill_region_ = -1;
  std::size_t fill_done_bytes_ = 0;
  const PositionSnapshot* fill_source_ = nullptr;
  std::array<gl::Fence, kSlots> fences_;
  std::array<bool, kSlots> readback_valid_{};

  // Render targets.
  gl::Framebuffer main_fbo_;
  gl::Texture main_color_, main_depth_;
  int target_w_ = 0, target_h_ = 0;
  Picker picker_;
  HudRenderer hud_;
  GpuTimers timers_;

  // Highlight state (CPU master copy, generation per slot).
  std::vector<std::uint32_t> highlight_master_;
  std::array<std::uint64_t, kSlots> highlight_gen_of_slot_{};
  std::uint64_t highlight_gen_ = 1;
  std::optional<std::uint32_t> selected_;

  // CPU copy of the latest snapshot positions (for fly-to / framing).
  const PositionSnapshot* latest_ = nullptr;

  std::size_t node_count_ = 0;
  std::size_t bundle_count_ = 0;
  std::uint32_t intra_count_ = 0, inter_count_ = 0;
  std::uint32_t intra_lod_ = 0, inter_lod_ = 0;
  std::uint32_t intra_cap_ = 0, inter_cap_ = 0;
  std::uint64_t frame_ = 0;
  unsigned slot_ = 0;
  RenderCounters counters_;
};

}  // namespace ftv
