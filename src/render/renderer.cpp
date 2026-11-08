#include "render/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "core/log.hpp"
#include "core/timing.hpp"
#include "ingest/types.hpp"
#include "render/shader_program.hpp"

namespace ftv {

namespace {

struct GpuEdge {
  std::uint32_t src;
  std::uint32_t dst;
  std::uint32_t attr;
  float weight;  // log-normalised [0, 1] within the edge's currency
};
static_assert(sizeof(GpuEdge) == 16);

struct GpuHub {
  float anchor_radius[4];
  float color[4];
  float params[4];
};
static_assert(sizeof(GpuHub) == 48);

struct GpuBundle {
  std::uint32_t ids[4];
  float params[4];
};
static_assert(sizeof(GpuBundle) == 32);

#pragma pack(push, 1)
struct NodeStatic {
  std::uint16_t hub;
  std::uint16_t flags;
  float size;
  std::uint32_t color_index;
};
#pragma pack(pop)
static_assert(sizeof(NodeStatic) == 12);

struct DrawArraysIndirectCommand {
  GLuint count;
  GLuint instanceCount;
  GLuint first;
  GLuint baseInstance;
};

void hsv_to_rgb(float h, float s, float v, float* rgb) {
  const float c = v * s;
  const float hp = std::fmod(h * 6.0f, 6.0f);
  const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
  float r = 0, g = 0, b = 0;
  if (hp < 1) { r = c; g = x; }
  else if (hp < 2) { r = x; g = c; }
  else if (hp < 3) { g = c; b = x; }
  else if (hp < 4) { g = x; b = c; }
  else if (hp < 5) { r = x; b = c; }
  else { r = c; b = x; }
  const float m = v - c;
  rgb[0] = r + m;
  rgb[1] = g + m;
  rgb[2] = b + m;
}

}  // namespace

Renderer::Renderer(const Graph& graph, const HubTaxonomy& taxonomy, const std::vector<CorridorBundle>& bundles,
                   const RenderSettings& settings, const GlCaps& caps, std::string shaderDir)
    : graph_(graph),
      taxonomy_(taxonomy),
      bundles_(bundles),
      settings_(settings),
      caps_(caps),
      shader_dir_(std::move(shaderDir)),
      node_count_(graph.node_count()) {
  if (!caps_.has_compute) settings_.gpu_culling = false;
}

Renderer::~Renderer() = default;

bool Renderer::create_programs(std::string& error) {
  auto make = [&](gl::Program& out, std::initializer_list<ShaderStage> stages) {
    auto p = build_program(shader_dir_, stages, error);
    if (!p) return false;
    out = std::move(*p);
    return true;
  };
  if (!make(prog_node_, {{GL_VERTEX_SHADER, "node.vert"}, {GL_FRAGMENT_SHADER, "node.frag"}})) return false;
  if (!make(prog_pick_, {{GL_VERTEX_SHADER, "pick.vert"}, {GL_FRAGMENT_SHADER, "pick.frag"}})) return false;
  if (!make(prog_hub_, {{GL_VERTEX_SHADER, "hub.vert"}, {GL_FRAGMENT_SHADER, "hub.frag"}})) return false;
  if (!make(prog_bundle_, {{GL_VERTEX_SHADER, "bundle.vert"}, {GL_FRAGMENT_SHADER, "bundle.frag"}})) return false;
  if (!make(prog_edge_, {{GL_VERTEX_SHADER, "edge.vert"}, {GL_FRAGMENT_SHADER, "edge.frag"}})) return false;
  if (!make(prog_hud_, {{GL_VERTEX_SHADER, "hud.vert"}, {GL_FRAGMENT_SHADER, "hud.frag"}})) return false;
  if (settings_.gpu_culling) {
    if (!make(prog_cull_, {{GL_COMPUTE_SHADER, "edge_cull.comp"}})) return false;
    if (!make(prog_clamp_, {{GL_COMPUTE_SHADER, "edge_clamp.comp"}})) return false;
    const char* names[7] = {"u_edge_count", "u_intra_count", "u_intra_lod", "u_inter_lod",
                            "u_intra_cap",  "u_inter_cap",   "u_detail"};
    for (int i = 0; i < 7; ++i) loc_cull_[i] = glGetUniformLocation(prog_cull_.id(), names[i]);
    loc_clamp_[0] = glGetUniformLocation(prog_clamp_.id(), "u_intra_cap");
    loc_clamp_[1] = glGetUniformLocation(prog_clamp_.id(), "u_inter_cap");
  }
  loc_edge_inter_ = glGetUniformLocation(prog_edge_.id(), "u_inter");
  loc_edge_direct_ = glGetUniformLocation(prog_edge_.id(), "u_direct");
  loc_edge_first_ = glGetUniformLocation(prog_edge_.id(), "u_first");
  loc_bundle_segments_ = glGetUniformLocation(prog_bundle_.id(), "u_segments");
  return true;
}

void Renderer::build_palette() {
  std::array<std::uint8_t, 256 * 4> px{};
  auto put = [&px](std::size_t i, float r, float g, float b) {
    px[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
    px[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
    px[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
    px[i * 4 + 3] = 255;
  };
  put(0, 0.6f, 0.6f, 0.6f);
  // Fixed colours for the major currencies keep the palette stable across
  // datasets; everything else gets golden-angle hues.
  const std::pair<const char*, std::array<float, 3>> fixed[] = {
      {"USD", {0.35f, 0.95f, 0.45f}}, {"EUR", {0.35f, 0.65f, 1.0f}},  {"GBP", {0.75f, 0.5f, 1.0f}},
      {"JPY", {1.0f, 0.4f, 0.4f}},    {"CHF", {1.0f, 0.45f, 0.7f}},   {"CNY", {1.0f, 0.3f, 0.25f}},
      {"HKD", {1.0f, 0.85f, 0.3f}},   {"SGD", {1.0f, 0.65f, 0.25f}},  {"BTC", {1.0f, 0.62f, 0.1f}},
      {"ETH", {0.62f, 0.66f, 1.0f}},  {"USDT", {0.2f, 0.85f, 0.7f}},  {"USDC", {0.3f, 0.6f, 0.95f}},
      {"SOL", {0.75f, 0.35f, 1.0f}},  {"TRX", {1.0f, 0.25f, 0.35f}},
  };
  for (std::size_t i = 0; i < graph_.currencies.size() && i < 127; ++i) {
    const std::uint32_t code = graph_.currencies[i];
    bool done = false;
    for (const auto& f : fixed) {
      if (pack_code4(f.first) == code) {
        put(i + 1, f.second[0], f.second[1], f.second[2]);
        done = true;
        break;
      }
    }
    if (!done) {
      float rgb[3];
      hsv_to_rgb(std::fmod(0.11f + static_cast<float>(i) * 0.618034f, 1.0f), 0.55f, 1.0f, rgb);
      put(i + 1, rgb[0], rgb[1], rgb[2]);
    }
  }
  const float channelColors[8][3] = {{1.0f, 0.8f, 0.3f},  {0.3f, 0.6f, 1.0f}, {0.35f, 0.95f, 0.45f},
                                     {0.3f, 0.9f, 0.9f},  {0.75f, 0.5f, 1.0f}, {1.0f, 0.4f, 0.4f},
                                     {1.0f, 0.55f, 0.15f}, {0.7f, 0.7f, 0.75f}};
  for (std::size_t c = 0; c < 8; ++c) put(128 + c, channelColors[c][0], channelColors[c][1], channelColors[c][2]);
  palette_ = gl::create_texture(GL_TEXTURE_1D);
  glTextureStorage1D(palette_.id(), 1, GL_RGBA8, 256);
  glTextureSubImage1D(palette_.id(), 0, 0, 256, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glTextureParameteri(palette_.id(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTextureParameteri(palette_.id(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void Renderer::upload_static_data() {
  const std::size_t N = node_count_;

  // ---- per-node static attributes (immutable VBO, instanced) ---------------
  {
    std::vector<NodeStatic> st(std::max<std::size_t>(N, 1));
    for (std::size_t i = 0; i < N; ++i) {
      st[i] = NodeStatic{graph_.nodes.hub[i], graph_.nodes.flags[i], graph_.nodes.size[i], graph_.nodes.hub[i]};
    }
    node_static_ = gl::create_buffer();
    glNamedBufferStorage(node_static_.id(), static_cast<GLsizeiptr>(st.size() * sizeof(NodeStatic)), st.data(), 0);
    vao_nodes_ = gl::create_vertex_array();
    const GLuint vao = vao_nodes_.id();
    glVertexArrayVertexBuffer(vao, 0, node_static_.id(), 0, sizeof(NodeStatic));
    glVertexArrayBindingDivisor(vao, 0, 1);  // one element per instance (glVertexAttribDivisor equivalent)
    glVertexArrayAttribIFormat(vao, 0, 1, GL_UNSIGNED_SHORT, static_cast<GLuint>(offsetof(NodeStatic, hub)));
    glVertexArrayAttribIFormat(vao, 1, 1, GL_UNSIGNED_SHORT, static_cast<GLuint>(offsetof(NodeStatic, flags)));
    glVertexArrayAttribFormat(vao, 2, 1, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offsetof(NodeStatic, size)));
    glVertexArrayAttribIFormat(vao, 3, 1, GL_UNSIGNED_INT, static_cast<GLuint>(offsetof(NodeStatic, color_index)));
    for (GLuint a = 0; a < 4; ++a) {
      glEnableVertexArrayAttrib(vao, a);
      glVertexArrayAttribBinding(vao, a, 0);
    }
  }
  vao_empty_ = gl::create_vertex_array();

  // ---- edges: validated, intra first then inter, each by priority ----------
  {
    const EdgeList& e = graph_.edges;
    // Priority = weight relative to the largest edge of the same currency (no
    // FX conversion offline). Computed once per edge so the sorts below only
    // compare two doubles.
    const std::vector<std::uint32_t>& cur = graph_.currencies;  // sorted, distinct
    std::vector<std::uint32_t> currencyRank(e.size());
    std::vector<double> maxPerCurrency(cur.size() + 1, 0.0);
    for (std::size_t i = 0; i < e.size(); ++i) {
      const auto it = std::lower_bound(cur.begin(), cur.end(), e.currency[i]);
      const auto r = static_cast<std::uint32_t>(
          (it != cur.end() && *it == e.currency[i]) ? static_cast<std::size_t>(it - cur.begin()) : cur.size());
      currencyRank[i] = r;
      maxPerCurrency[r] = std::max(maxPerCurrency[r], std::fabs(static_cast<double>(e.weight[i])));
    }
    std::vector<double> priority(e.size());
    for (std::size_t i = 0; i < e.size(); ++i) {
      const double m = maxPerCurrency[currencyRank[i]];
      priority[i] = m > 0 ? std::fabs(static_cast<double>(e.weight[i])) / m : 0.0;
    }
    std::vector<std::uint32_t> intra, inter;
    std::uint64_t invalid = 0;
    for (std::size_t i = 0; i < e.size(); ++i) {
      if (e.src[i] >= N || e.dst[i] >= N) {
        ++invalid;  // never upload an index that could read out of bounds
        continue;
      }
      if (e.flags[i] & kEdgeSelfLoop) continue;
      ((e.flags[i] & kEdgeInterHub) ? inter : intra).push_back(static_cast<std::uint32_t>(i));
    }
    if (invalid) FTV_LOG_WARN("renderer skipped %llu edges with invalid indices", static_cast<unsigned long long>(invalid));
    // Descending priority; ties by edge index, so the order is deterministic.
    auto byPrio = [&priority](std::uint32_t a, std::uint32_t b) {
      const double pa = priority[a], pb = priority[b];
      return pa != pb ? pa > pb : a < b;
    };
    std::sort(intra.begin(), intra.end(), byPrio);
    std::sort(inter.begin(), inter.end(), byPrio);
    intra_count_ = static_cast<std::uint32_t>(intra.size());
    inter_count_ = static_cast<std::uint32_t>(inter.size());

    std::vector<GpuEdge> gpu;
    gpu.reserve(intra.size() + inter.size() + 1);
    auto pack = [&](std::uint32_t i) {
      const double m = maxPerCurrency[currencyRank[i]];
      const double w = std::fabs(static_cast<double>(e.weight[i]));
      const float wn = m > 0 ? static_cast<float>(std::log1p(w) / std::log1p(m)) : 0.0f;
      const std::uint32_t hs = graph_.nodes.hub[e.src[i]] & 0xFFu;
      const std::uint32_t hd = graph_.nodes.hub[e.dst[i]] & 0xFFu;
      const std::uint32_t attr = (graph_.currency_palette_index(e.currency[i]) & 0xFFu) |
                                 ((static_cast<std::uint32_t>(e.channel[i]) & 0xFu) << 8) |
                                 ((static_cast<std::uint32_t>(e.flags[i]) & 0xFu) << 12) | (hs << 16) | (hd << 24);
      gpu.push_back(GpuEdge{e.src[i], e.dst[i], attr, wn});
    };
    for (auto i : intra) pack(i);
    for (auto i : inter) pack(i);
    if (gpu.empty()) gpu.push_back(GpuEdge{0, 0, 0, 0.0f});
    edges_ = gl::create_buffer();
    glNamedBufferStorage(edges_.id(), static_cast<GLsizeiptr>(gpu.size() * sizeof(GpuEdge)), gpu.data(), 0);

    // Visible-edge budget (<= 2M drawn edges per frame, by weight priority):
    //   * intra-hub edges: the top (1 - percentile) by priority, i.e. a
    //     prefix of the sorted intra range;
    //   * inter-hub edges: a prefix of the sorted inter range, shown
    //     individually only near the camera or for the selected node;
    //   * `reserve` extra slots keep the selected node's edges visible even
    //     when they rank below the LOD prefix.
    // intra_cap_ + inter_cap_ <= budget always holds, and the clamp shader
    // limits the indirect counts to the caps, so no frame ever draws more.
    const std::uint32_t budget = std::min(settings_.max_visible_edges, kMaxVisibleEdges);
    const std::uint32_t reserve = std::min<std::uint32_t>(65536u, budget / 8u);
    const auto keep = static_cast<std::uint32_t>(
        std::floor(static_cast<double>(intra_count_) * (1.0 - std::clamp(settings_.intra_edge_lod_percentile, 0.0f, 0.999f))));
    inter_lod_ = std::min({inter_count_, settings_.max_inter_hub_edges, (budget - reserve) / 4u});
    intra_lod_ = std::min({intra_count_, keep, budget - reserve - inter_lod_});
    inter_cap_ = std::min(inter_count_, inter_lod_ + reserve / 2u);
    intra_cap_ = std::min(intra_count_, budget - inter_cap_);
    visible_intra_ = gl::create_buffer();
    glNamedBufferStorage(visible_intra_.id(), std::max<GLsizeiptr>(4, static_cast<GLsizeiptr>(intra_cap_) * 4), nullptr, 0);
    visible_inter_ = gl::create_buffer();
    glNamedBufferStorage(visible_inter_.id(), std::max<GLsizeiptr>(4, static_cast<GLsizeiptr>(inter_cap_) * 4), nullptr, 0);
    const DrawArraysIndirectCommand cmds[2] = {{2, 0, 0, 0}, {8, 0, 0, 0}};
    indirect_ = gl::create_buffer();
    glNamedBufferStorage(indirect_.id(), sizeof(cmds), cmds, 0);
  }

  // ---- hubs and corridor bundles ---------------------------------------------
  {
    std::vector<GpuHub> hubs;
    for (std::size_t h = 0; h < taxonomy_.size(); ++h) {
      const HubDef& d = taxonomy_.hubs()[h];
      const float pop = h < graph_.hub_population.size() ? static_cast<float>(graph_.hub_population[h]) : 0.0f;
      hubs.push_back(GpuHub{{d.anchor.x, d.anchor.y, d.anchor.z, d.radius},
                            {d.color.x, d.color.y, d.color.z, d.color.w},
                            {static_cast<float>(d.type), d.k_anchor, std::log1p(pop), 0.0f}});
    }
    hubs_ = gl::create_buffer();
    glNamedBufferStorage(hubs_.id(), static_cast<GLsizeiptr>(hubs.size() * sizeof(GpuHub)), hubs.data(), 0);

    std::vector<GpuBundle> b;
    for (const CorridorBundle& c : bundles_) {
      b.push_back(GpuBundle{{c.hub_a, c.hub_b, c.palette, 0u}, {c.thickness, c.brightness, 0.0f, 0.0f}});
    }
    bundle_count_ = b.size();
    if (b.empty()) b.push_back(GpuBundle{});
    bundles_buf_ = gl::create_buffer();
    glNamedBufferStorage(bundles_buf_.id(), static_cast<GLsizeiptr>(b.size() * sizeof(GpuBundle)), b.data(), 0);
  }
  build_palette();
}

bool Renderer::init(std::string& error) {
  Stopwatch initClock;
  if (!create_programs(error)) return false;
  const double programsMs = initClock.lap_ms();
  upload_static_data();
  const double staticMs = initClock.lap_ms();

  const std::size_t N = std::max<std::size_t>(node_count_, 1);
  pos_region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(N * 3 * sizeof(float)), caps_.ssbo_alignment);
  ubo_region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(sizeof(FrameUniforms)), caps_.ubo_alignment);
  const std::size_t words = (N + 31) / 32;
  hl_region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(words * 4), caps_.ssbo_alignment);
  if (!positions_.create(pos_region_bytes_ * kSlots, GL_MAP_WRITE_BIT) ||
      !uniforms_.create(ubo_region_bytes_ * kSlots, GL_MAP_WRITE_BIT) ||
      !highlight_.create(hl_region_bytes_ * kSlots, GL_MAP_WRITE_BIT) ||
      !readback_.create(sizeof(DrawArraysIndirectCommand) * 2 * kSlots, GL_MAP_READ_BIT)) {
    error = "failed to create persistently mapped buffers";
    return false;
  }
  std::memset(positions_.data(), 0, static_cast<std::size_t>(positions_.size()));
  std::memset(highlight_.data(), 0, static_cast<std::size_t>(highlight_.size()));
  highlight_master_.assign(words, 0u);
  if (!picker_.create() || !hud_.create(caps_.ssbo_alignment)) {
    error = "failed to create picking / HUD buffers";
    return false;
  }
  timers_.create();

  glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);  // reverse-Z needs [0,1] clip depth
  glEnable(GL_PROGRAM_POINT_SIZE);
  glDisable(GL_CULL_FACE);
  FTV_LOG_INFO("renderer: %zu nodes, %u intra + %u inter edges (LOD %u / %u), %zu corridor bundles, culling=%s; "
               "init %.0f ms (shaders %.0f, static data %.0f)",
               node_count_, intra_count_, inter_count_, intra_lod_, inter_lod_, bundle_count_,
               settings_.gpu_culling ? "gpu-compute" : "vertex-shader fallback", programsMs + staticMs + initClock.lap_ms(),
               programsMs, staticMs);
  return true;
}

void Renderer::ensure_targets(int width, int height) {
  if (width == target_w_ && height == target_h_ && main_fbo_) return;
  target_w_ = width;
  target_h_ = height;
  const int samples = std::max(1, settings_.msaa_samples);
  main_fbo_ = gl::create_framebuffer();
  if (samples > 1) {
    main_color_ = gl::create_texture(GL_TEXTURE_2D_MULTISAMPLE);
    main_depth_ = gl::create_texture(GL_TEXTURE_2D_MULTISAMPLE);
    glTextureStorage2DMultisample(main_color_.id(), samples, GL_RGBA8, width, height, GL_TRUE);
    glTextureStorage2DMultisample(main_depth_.id(), samples, GL_DEPTH_COMPONENT32F, width, height, GL_TRUE);
  } else {
    main_color_ = gl::create_texture(GL_TEXTURE_2D);
    main_depth_ = gl::create_texture(GL_TEXTURE_2D);
    glTextureStorage2D(main_color_.id(), 1, GL_RGBA8, width, height);
    glTextureStorage2D(main_depth_.id(), 1, GL_DEPTH_COMPONENT32F, width, height);
  }
  glNamedFramebufferTexture(main_fbo_.id(), GL_COLOR_ATTACHMENT0, main_color_.id(), 0);
  glNamedFramebufferTexture(main_fbo_.id(), GL_DEPTH_ATTACHMENT, main_depth_.id(), 0);
  glNamedFramebufferDrawBuffer(main_fbo_.id(), GL_COLOR_ATTACHMENT0);
  const GLenum status = glCheckNamedFramebufferStatus(main_fbo_.id(), GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) FTV_LOG_ERROR("main framebuffer incomplete: 0x%x", status);
}

void Renderer::begin_frame(TripleBuffer<PositionSnapshot>& snapshots) {
  slot_ = static_cast<unsigned>(frame_ % kSlots);
  // Synchronisation point: the GPU has finished frame (f - 3), so this slot's
  // UBO / highlight / HUD regions, its timer queries and its readback copy
  // are free to reuse.
  Stopwatch sw;
  if (!fences_[slot_].wait(1'000'000'000ull)) FTV_LOG_WARN("frame fence wait timed out (GPU hang or reset?)");
  counters_.fence_wait_ms = static_cast<float>(sw.lap_ms());
  counters_.upload_ms = 0.0f;
  timers_.collect(slot_);
  if (readback_valid_[slot_]) {
    DrawArraysIndirectCommand c[2];
    std::memcpy(c, static_cast<const char*>(readback_.data()) + slot_ * sizeof(c), sizeof(c));
    counters_.visible_intra = std::min(c[0].instanceCount, intra_cap_);
    counters_.visible_inter = std::min(c[1].instanceCount, inter_cap_);
  }

  // Layout snapshot upload, amortised over frames (kUploadBytesPerFrame).
  if (fill_region_ < 0 && snapshots.acquire_latest()) {
    // Start filling a region not referenced by the two frames that may still
    // be in flight (3 regions, at most 2 busy => one is always free). Frames
    // keep drawing the current region until the fill completes, so the
    // target stays unreferenced for the whole fill.
    fill_source_ = &snapshots.front();
    latest_ = fill_source_;  // CPU-side lookups (framing, HUD) use the newest positions
    const unsigned busyA = pos_region_of_slot_[(slot_ + 1) % kSlots];
    const unsigned busyB = pos_region_of_slot_[(slot_ + 2) % kSlots];
    unsigned r = 0;
    while (r == busyA || r == busyB) ++r;
    fill_region_ = static_cast<int>(r);
    fill_done_bytes_ = 0;
  }
  if (fill_region_ >= 0) {
    const std::size_t total = std::min(fill_source_->xyz.size(), node_count_ * 3) * sizeof(float);
    const std::size_t chunk = std::min(kUploadBytesPerFrame, total - fill_done_bytes_);
    std::memcpy(static_cast<char*>(positions_.data()) +
                    static_cast<std::size_t>(pos_region_bytes_) * static_cast<std::size_t>(fill_region_) +
                    fill_done_bytes_,
                reinterpret_cast<const char*>(fill_source_->xyz.data()) + fill_done_bytes_, chunk);
    fill_done_bytes_ += chunk;
    if (fill_done_bytes_ >= total) {
      pos_region_ = static_cast<unsigned>(fill_region_);  // complete: draw from it from this frame on
      fill_region_ = -1;
      counters_.snapshot_iteration = fill_source_->iteration;
      ++counters_.snapshots_uploaded;
    }
    counters_.upload_ms = static_cast<float>(sw.lap_ms());
  }
  pos_region_of_slot_[slot_] = pos_region_;
  hud_.begin(slot_);
}

void Renderer::prime_positions(const PositionSnapshot& snapshot) {
  // Only valid before the first frame of this renderer (no frame in flight).
  latest_ = &snapshot;
  const std::size_t bytes = std::min(snapshot.xyz.size(), node_count_ * 3) * sizeof(float);
  for (unsigned r = 0; r < kSlots; ++r) {
    std::memcpy(static_cast<char*>(positions_.data()) + static_cast<std::size_t>(pos_region_bytes_) * r,
                snapshot.xyz.data(), bytes);
  }
  counters_.snapshot_iteration = snapshot.iteration;
}

void Renderer::refresh_highlight_region(unsigned slot) noexcept {
  if (highlight_gen_of_slot_[slot] == highlight_gen_) return;
  std::memcpy(static_cast<char*>(highlight_.data()) + static_cast<std::size_t>(hl_region_bytes_) * slot,
              highlight_master_.data(), highlight_master_.size() * sizeof(std::uint32_t));
  highlight_gen_of_slot_[slot] = highlight_gen_;
}

void Renderer::set_selection(std::optional<std::uint32_t> node) {
  if (node && *node >= node_count_) node.reset();
  if (node == selected_) return;
  const auto& adj = graph_.adjacency;
  auto apply = [&](std::uint32_t n, bool set) {
    auto bit = [&](std::uint32_t i) {
      if (set) {
        highlight_master_[i >> 5] |= 1u << (i & 31u);
      } else {
        highlight_master_[i >> 5] &= ~(1u << (i & 31u));
      }
    };
    bit(n);
    for (std::uint32_t k = adj.offset[n]; k < adj.offset[n + 1]; ++k) bit(adj.neighbor[k]);
  };
  if (selected_) apply(*selected_, false);
  if (node) apply(*node, true);
  selected_ = node;
  ++highlight_gen_;
}

glm::vec3 Renderer::node_position(std::uint32_t node) const noexcept {
  if (latest_ && static_cast<std::size_t>(node) * 3 + 2 < latest_->xyz.size()) {
    return {latest_->xyz[node * 3u], latest_->xyz[node * 3u + 1], latest_->xyz[node * 3u + 2]};
  }
  if (node < node_count_) return taxonomy_.hubs()[graph_.nodes.hub[node]].anchor;
  return glm::vec3(0.0f);
}

void Renderer::draw_frame(const FrameParams& p) {
  const int w = p.width, h = p.height;
  ensure_targets(w, h);
  counters_.draw_calls = 0;

  // ---- per-frame uniforms (persistently mapped ring, no buffer updates) ----
  FrameUniforms u;
  u.view = p.view;
  u.proj = p.proj;
  u.view_proj = p.proj * p.view;
  u.camera = glm::vec4(p.camera_pos, p.time);
  u.viewport = glm::vec4(static_cast<float>(w), static_cast<float>(h), 1.0f / static_cast<float>(w),
                         1.0f / static_cast<float>(h));
  u.params0 = glm::vec4(settings_.node_size_scale, settings_.edge_alpha, settings_.inter_edge_alpha,
                        settings_.corridor_width_px);
  u.params1 = glm::vec4(settings_.inter_hub_detail_distance, settings_.hub_glow_strength,
                        p.color_by_channel ? 1.0f : 0.0f, p.near_plane);
  u.counts = glm::uvec4(static_cast<std::uint32_t>(node_count_), selected_ ? *selected_ + 1u : 0u,
                        static_cast<std::uint32_t>(taxonomy_.size()), settings_.hub_particles_per_hub);
  std::memcpy(static_cast<char*>(uniforms_.data()) + static_cast<std::size_t>(ubo_region_bytes_) * slot_, &u, sizeof(u));
  refresh_highlight_region(slot_);

  glBindBufferRange(GL_UNIFORM_BUFFER, 0, uniforms_.id(), ubo_region_bytes_ * slot_, sizeof(FrameUniforms));
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, positions_.id(), pos_region_bytes_ * pos_region_, pos_region_bytes_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, edges_.id());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, hubs_.id());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, bundles_buf_.id());
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 4, highlight_.id(), hl_region_bytes_ * slot_, hl_region_bytes_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, indirect_.id());
  glBindTextureUnit(0, palette_.id());

  const std::uint32_t edgeTotal = intra_count_ + inter_count_;

  // ---- compute: edge culling + compaction ----------------------------------
  if (settings_.gpu_culling && edgeTotal > 0) {
    timers_.begin(slot_, GpuPass::Cull);
    const GLuint zero = 0;
    glClearNamedBufferSubData(indirect_.id(), GL_R32UI, offsetof(DrawArraysIndirectCommand, instanceCount), 4,
                              GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glClearNamedBufferSubData(indirect_.id(), GL_R32UI,
                              sizeof(DrawArraysIndirectCommand) + offsetof(DrawArraysIndirectCommand, instanceCount), 4,
                              GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glUseProgram(prog_cull_.id());
    glUniform1ui(loc_cull_[0], edgeTotal);
    glUniform1ui(loc_cull_[1], intra_count_);
    glUniform1ui(loc_cull_[2], intra_lod_);
    glUniform1ui(loc_cull_[3], inter_lod_);
    glUniform1ui(loc_cull_[4], intra_cap_);
    glUniform1ui(loc_cull_[5], inter_cap_);
    glUniform1ui(loc_cull_[6], p.inter_detail ? 1u : 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, visible_intra_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, visible_inter_.id());
    glDispatchCompute((edgeTotal + 255u) / 256u, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glUseProgram(prog_clamp_.id());
    glUniform1ui(loc_clamp_[0], intra_cap_);
    glUniform1ui(loc_clamp_[1], inter_cap_);
    glDispatchCompute(1, 1, 1);
    // The indirect draws read the counts, the edge VS reads the lists.
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    timers_.end();
  }

  // ---- main target -----------------------------------------------------------
  glBindFramebuffer(GL_FRAMEBUFFER, main_fbo_.id());
  glViewport(0, 0, w, h);
  const GLfloat bg[4] = {settings_.background[0], settings_.background[1], settings_.background[2], 1.0f};
  const GLfloat depth0 = 0.0f;  // reverse-Z: 0 is the far plane
  glClearNamedFramebufferfv(main_fbo_.id(), GL_COLOR, 0, bg);
  glClearNamedFramebufferfv(main_fbo_.id(), GL_DEPTH, 0, &depth0);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GREATER);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);

  // Pass 4 - nodes (opaque, writes depth).
  if (node_count_ > 0) {
    timers_.begin(slot_, GpuPass::Nodes);
    glUseProgram(prog_node_.id());
    glBindVertexArray(vao_nodes_.id());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(node_count_));
    ++counters_.draw_calls;
    timers_.end();
  }

  // Translucent passes: depth test on, depth write off.
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBindVertexArray(vao_empty_.id());

  // Pass 1 - hub clouds (additive, premultiplied).
  timers_.begin(slot_, GpuPass::Hubs);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(prog_hub_.id());
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                        static_cast<GLsizei>(taxonomy_.size() * settings_.hub_particles_per_hub));
  ++counters_.draw_calls;
  timers_.end();

  // Pass 2 - corridor bundles (premultiplied alpha "over": overlapping
  // ribbons keep their currency colour instead of saturating to white).
  if (bundle_count_ > 0) {
    timers_.begin(slot_, GpuPass::Corridors);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_bundle_.id());
    glUniform1i(loc_bundle_segments_, kCorridorSegments);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4 * kCorridorSegments, static_cast<GLsizei>(bundle_count_));
    ++counters_.draw_calls;
    timers_.end();
  }

  // Pass 3 - edges (alpha blended).
  if (edgeTotal > 0) {
    timers_.begin(slot_, GpuPass::Edges);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_edge_.id());
    if (settings_.gpu_culling) {
      glUniform1ui(loc_edge_direct_, 0u);
      glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect_.id());
      glUniform1ui(loc_edge_inter_, 0u);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, visible_intra_.id());
      glDrawArraysIndirect(GL_LINES, reinterpret_cast<const void*>(0));
      glUniform1ui(loc_edge_inter_, 1u);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, visible_inter_.id());
      glDrawArraysIndirect(GL_LINE_STRIP, reinterpret_cast<const void*>(sizeof(DrawArraysIndirectCommand)));
      glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
      counters_.draw_calls += 2;
    } else {
      // GL 4.3 fallback without compute culling: draw the LOD prefix directly
      // and let the vertex shader collapse culled edges.
      glUniform1ui(loc_edge_direct_, 1u);
      glUniform1ui(loc_edge_inter_, 0u);
      glUniform1ui(loc_edge_first_, 0u);
      if (intra_lod_ > 0) {
        glDrawArraysInstanced(GL_LINES, 0, 2, static_cast<GLsizei>(intra_lod_));
        ++counters_.draw_calls;
      }
      if (p.inter_detail && inter_lod_ > 0) {
        glUniform1ui(loc_edge_inter_, 1u);
        glUniform1ui(loc_edge_first_, intra_count_);
        glDrawArraysInstanced(GL_LINE_STRIP, 0, 8, static_cast<GLsizei>(inter_lod_));
        ++counters_.draw_calls;
      }
      counters_.visible_intra = intra_lod_;
      counters_.visible_inter = p.inter_detail ? inter_lod_ : 0u;
    }
    timers_.end();
  }

  // Pass 5 - picking, only when a click is pending.
  if (picker_.pending_render() && node_count_ > 0) {
    timers_.begin(slot_, GpuPass::Pick);
    picker_.resize(w, h);
    picker_.begin_pass();
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glUseProgram(prog_pick_.id());
    glBindVertexArray(vao_nodes_.id());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(node_count_));
    ++counters_.draw_calls;
    picker_.end_pass();
    timers_.end();
  }

  // Resolve / copy to the window.
  glBlitNamedFramebuffer(main_fbo_.id(), 0, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // Pass 6 - HUD, directly on the default framebuffer.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  if (p.show_hud) {
    timers_.begin(slot_, GpuPass::Hud);
    hud_.draw(prog_hud_.id());
    ++counters_.draw_calls;
    timers_.end();
  }
  glDepthMask(GL_TRUE);

  if (settings_.gpu_culling && edgeTotal > 0) {
    // Asynchronous readback of the visible counts for the HUD (read 3 frames later).
    glCopyNamedBufferSubData(indirect_.id(), readback_.id(), 0,
                             static_cast<GLintptr>(slot_ * 2 * sizeof(DrawArraysIndirectCommand)),
                             2 * sizeof(DrawArraysIndirectCommand));
    readback_valid_[slot_] = true;
  }
  fences_[slot_].insert();
  ++frame_;
}

bool Renderer::save_screenshot(const std::string& path, int width, int height) const {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadBuffer(GL_BACK);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int y = height - 1; y >= 0; --y) {
    std::fwrite(px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3, 1,
                static_cast<std::size_t>(width) * 3, f);
  }
  std::fclose(f);
  return true;
}

}  // namespace ftv
