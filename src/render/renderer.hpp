#pragma once
// The instanced GPU renderer. Owns every GL object and records the same
// draw calls every frame, whatever the size of the graph:
//
//   scene (8x MSAA, RGBA16F colour + RGBA16F emissive MRT + 32F depth)
//     Pass 1  globe + graticule  glDrawArrays(GL_TRIANGLES, 0, 3)            fullscreen ray cast
//     Pass 2  nodes              glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)
//     Pass 3  arcs               glDrawArraysInstanced(LINES, 0, 2, intra)
//                                glDrawArraysInstanced(LINE_STRIP, 0, S+1, inter)
//     Pass 4  whales (emissive)  glDrawArraysInstanced(TRIANGLE_STRIP, 0, 2(S+1), 2W)
//     Pass 5  hub markers+labels glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, H+G)
//     Pass 6  picking            glDrawArraysInstanced (only on click)
//   post
//     MSAA resolve (2 blits), bloom on the emissive attachment only:
//     5 dual-filter downsamples + 4 upsamples, composite (ACES fitted),
//     HUD, present: all fullscreen glDrawArrays(GL_TRIANGLES, 0, 3) or one
//     instanced HUD draw.
// No glDrawElements*, no indirect draws.
//
// Per-frame CPU work: one fence wait, an optional chunk of a new layout
// snapshot copied into a free region of the persistently mapped position
// ring, a 464-byte uniform write, HUD glyph writes, and the draw calls. No
// buffer (re)specification, no glBufferData/SubData, no heap allocation.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/triple_buffer.hpp"
#include "layout/layout_engine.hpp"
#include "model/globe.hpp"
#include "model/graph.hpp"
#include "model/hub_taxonomy.hpp"
#include "model/whales.hpp"
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
  float time = 0.0f;  // replay clock (s)
  float near_plane = 0.5f;
  int width = 0;
  int height = 0;
  bool inter_detail = true;  // E key
  bool show_hud = true;
  bool present = true;       // copy the output to the default framebuffer (false: offscreen only)
};

struct RenderCounters {
  std::uint32_t visible_intra = 0;  // intra arcs submitted this frame
  std::uint32_t visible_inter = 0;
  std::uint32_t scene_draws = 0;    // instanced scene-geometry draws
  std::uint32_t post_draws = 0;     // fullscreen + HUD draws
  std::uint32_t draw_calls = 0;     // total
  std::uint32_t active_whales = 0;  // whale events in flight or flashing (CPU-side count)
  std::uint64_t snapshot_iteration = 0;
  std::uint64_t snapshots_uploaded = 0;
  float fence_wait_ms = 0.0f;  // CPU time blocked on the frame-slot fence (GPU-bound time)
  float upload_ms = 0.0f;      // CPU time copying a new layout snapshot into the ring (0 when none)
};

class Renderer {
 public:
  static constexpr unsigned kSlots = 3;
  static constexpr int kBloomLevels = 5;
  static constexpr std::uint32_t kMaxVisibleEdges = 2000000;  // hard cap on drawn edges per frame
  // Most bytes of a layout snapshot copied into the position ring per frame,
  // so a 1M-node snapshot (12 MB) is spread over 3 frames.
  static constexpr std::size_t kUploadBytesPerFrame = std::size_t{4} << 20;

  Renderer(const Graph& graph, const HubTaxonomy& taxonomy, const WhaleSet& whales, const RenderSettings& settings,
           const GlCaps& caps, std::string shaderDir);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool init(std::string& error);

  // Frame protocol: begin_frame() waits for the slot fence and consumes the
  // newest layout snapshot; the caller then writes HUD text via hud(); then
  // draw_frame() records all passes and fences the slot.
  void begin_frame(TripleBuffer<PositionSnapshot>& snapshots);
  // Copies `snapshot` into every position region (right after init()).
  void prime_positions(const PositionSnapshot& snapshot);
  HudRenderer& hud() noexcept { return hud_; }
  void draw_frame(const FrameParams& params);

  // Selection highlight (1-hop neighbourhood) as bits in a small SSBO.
  void set_selection(std::optional<std::uint32_t> node);
  [[nodiscard]] std::optional<std::uint32_t> selection() const noexcept { return selected_; }

  void request_pick(int x, int y) noexcept { picker_.request(x, y); }
  bool poll_pick(std::optional<std::uint32_t>& node) noexcept { return picker_.poll(node); }

  // World (globe) position of a node from the CPU copy of the latest snapshot.
  [[nodiscard]] glm::vec3 node_world_position(std::uint32_t node) const noexcept;

  [[nodiscard]] const GpuTimers& timers() const noexcept { return timers_; }
  [[nodiscard]] const RenderCounters& counters() const noexcept { return counters_; }
  [[nodiscard]] std::uint32_t intra_edge_count() const noexcept { return intra_count_; }
  [[nodiscard]] std::uint32_t inter_edge_count() const noexcept { return inter_count_; }
  [[nodiscard]] std::uint32_t intra_budget() const noexcept { return intra_lod_; }
  [[nodiscard]] std::uint32_t inter_budget() const noexcept { return inter_lod_; }
  [[nodiscard]] std::uint32_t visible_capacity() const noexcept { return intra_lod_ + inter_lod_; }
  [[nodiscard]] std::size_t whale_count() const noexcept { return whale_count_; }
  [[nodiscard]] const RenderSettings& settings() const noexcept { return settings_; }

  // Read-backs of the last frame (startup, screenshot and tests only; they
  // allocate and stall). Rows are returned top to bottom.
  bool read_output_rgba8(std::vector<std::uint8_t>& rgba, int& width, int& height);
  bool read_emissive(std::vector<float>& rgba, int& width, int& height);

 private:
  struct FrameUniforms {  // std140, mirrors `Frame` in common.glsl
    glm::mat4 view, proj, view_proj, inv_view_proj;
    glm::vec4 camera;
    glm::vec4 viewport;
    glm::vec4 globe;
    glm::vec4 ink, amber, gold;
    glm::vec4 alpha0, alpha1;
    glm::vec4 shape0, shape1;
    glm::vec4 whale0, whale1;
    glm::uvec4 counts;
  };

  bool create_programs(std::string& error);
  void upload_static_data();
  void ensure_targets(int width, int height);
  void refresh_highlight_region(unsigned slot) noexcept;
  void fullscreen() noexcept;

  const Graph& graph_;
  const HubTaxonomy& taxonomy_;
  const WhaleSet& whales_;
  RenderSettings settings_;
  GlCaps caps_;
  std::string shader_dir_;
  std::vector<HubFrame> hub_frames_;

  // Programs.
  gl::Program prog_globe_, prog_node_, prog_arc_, prog_whale_, prog_hub_, prog_pick_, prog_hud_;
  gl::Program prog_down_, prog_up_, prog_composite_, prog_present_;
  GLint loc_arc_inter_ = -1, loc_arc_first_ = -1, loc_arc_segments_ = -1, loc_whale_segments_ = -1;
  GLint loc_down_texel_ = -1, loc_up_texel_ = -1;
  GLint loc_comp_bg_ = -1, loc_comp_exposure_ = -1, loc_comp_bloom_ = -1, loc_comp_vignette_ = -1;
  GLint loc_present_encode_ = -1;

  // Vertex arrays.
  gl::VertexArray vao_empty_;
  gl::VertexArray vao_nodes_;

  // Immutable data.
  gl::Buffer node_static_;
  gl::Buffer edges_;
  gl::Buffer hubs_;
  gl::Buffer labels_;
  gl::Buffer whales_buf_;
  gl::Texture label_atlas_;

  // Persistently mapped rings.
  gl::PersistentBuffer positions_;  // 3 regions of N*12 bytes
  gl::PersistentBuffer uniforms_;   // 3 regions
  gl::PersistentBuffer highlight_;  // 3 regions of ceil(N/32)*4 bytes
  GLsizeiptr pos_region_bytes_ = 0, ubo_region_bytes_ = 0, hl_region_bytes_ = 0;
  std::array<unsigned, kSlots> pos_region_of_slot_{};
  unsigned pos_region_ = 0;
  // Amortised snapshot upload: the region being filled from `fill_source_`
  // over several frames is not drawn from until complete.
  int fill_region_ = -1;
  std::size_t fill_done_bytes_ = 0;
  const PositionSnapshot* fill_source_ = nullptr;
  std::array<gl::Fence, kSlots> fences_;

  // Render targets.
  gl::Framebuffer scene_fbo_;
  gl::Texture scene_color_, scene_emissive_, scene_depth_;
  gl::Framebuffer resolve_color_fbo_, resolve_emissive_fbo_;
  gl::Texture resolved_color_, resolved_emissive_;
  std::array<gl::Framebuffer, kBloomLevels> down_fbo_;
  std::array<gl::Texture, kBloomLevels> down_tex_;
  std::array<gl::Framebuffer, kBloomLevels - 1> up_fbo_;
  std::array<gl::Texture, kBloomLevels - 1> up_tex_;
  std::array<int, kBloomLevels> level_w_{}, level_h_{};
  gl::Framebuffer output_fbo_;
  gl::Texture output_tex_;
  int target_w_ = 0, target_h_ = 0;
  bool default_fb_srgb_ = false;
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
  std::size_t whale_count_ = 0;
  std::uint32_t label_glyphs_ = 0;
  std::uint32_t intra_count_ = 0, inter_count_ = 0;
  std::uint32_t intra_lod_ = 0, inter_lod_ = 0;
  std::uint64_t frame_ = 0;
  unsigned slot_ = 0;
  RenderCounters counters_;
};

}  // namespace ftv
